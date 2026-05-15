// Generic C-ABI calling. Loads any shared library (no plugin header required)
// and lets bnl code call exported C functions by declaring their signature.
// Backed by libffi for cross-platform calling-convention handling.
//
// Phase 1 surface:
//   ffi.open(name)                              -> library handle
//   ffi.open_c()                                -> platform's C runtime library
//   ffi.close(handle)                           -> drop handle (refcount)
//   ffi.fn(lib, sym, ret_type, [arg_types])     -> bound bnl-callable
//
// Supported type names (strings): void, int8/16/32/64, uint8/16/32/64,
// float, double, ptr, cstr.
//
// Pointer representation: a plain Number containing the address. `null` is
// the null pointer (both ways). Users shouldn't do arithmetic on pointers —
// the type is documentary, not enforced beyond the marshaling layer.

#include "stdlib/registry.h"

#include "bnl/interpreter.h"
#include "bnl/native_module.h"
#include "bnl/value.h"
#include "ffi/dynamic_library.h"

#include <ffi.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bnl {

namespace {

// ---------- type table ------------------------------------------------------

enum class FfiKind {
    Void, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, Ptr, CStr
};

struct FfiType {
    FfiKind   kind;
    ffi_type* type;
};

bool try_type_from_name(const std::string& name, FfiType* out) {
    if (name == "void")   { *out = {FfiKind::Void, &ffi_type_void};    return true; }
    if (name == "int8")   { *out = {FfiKind::I8,   &ffi_type_sint8};   return true; }
    if (name == "int16")  { *out = {FfiKind::I16,  &ffi_type_sint16};  return true; }
    if (name == "int32")  { *out = {FfiKind::I32,  &ffi_type_sint32};  return true; }
    if (name == "int64")  { *out = {FfiKind::I64,  &ffi_type_sint64};  return true; }
    if (name == "uint8")  { *out = {FfiKind::U8,   &ffi_type_uint8};   return true; }
    if (name == "uint16") { *out = {FfiKind::U16,  &ffi_type_uint16};  return true; }
    if (name == "uint32") { *out = {FfiKind::U32,  &ffi_type_uint32};  return true; }
    if (name == "uint64") { *out = {FfiKind::U64,  &ffi_type_uint64};  return true; }
    if (name == "float")  { *out = {FfiKind::F32,  &ffi_type_float};   return true; }
    if (name == "double") { *out = {FfiKind::F64,  &ffi_type_double};  return true; }
    if (name == "ptr")    { *out = {FfiKind::Ptr,  &ffi_type_pointer}; return true; }
    if (name == "cstr")   { *out = {FfiKind::CStr, &ffi_type_pointer}; return true; }
    return false;
}

FfiType type_from_name(const std::string& name) {
    FfiType out;
    if (!try_type_from_name(name, &out))
        throw std::runtime_error("ffi: unknown type '" + name + "'");
    return out;
}

std::size_t size_of_kind(FfiKind k) {
    switch (k) {
        case FfiKind::Void:                       return 0;
        case FfiKind::I8:  case FfiKind::U8:      return 1;
        case FfiKind::I16: case FfiKind::U16:     return 2;
        case FfiKind::I32: case FfiKind::U32:
        case FfiKind::F32:                        return 4;
        case FfiKind::I64: case FfiKind::U64:
        case FfiKind::F64:                        return 8;
        case FfiKind::Ptr: case FfiKind::CStr:    return sizeof(void*);
    }
    return 0;
}

// Alignment = size for primitive types under the default C ABI. Works on
// every preset we target (x64 Windows/Linux/macOS, ARM64 macOS).
std::size_t align_of_kind(FfiKind k) { return size_of_kind(k); }

// ---------- struct registry -------------------------------------------------

struct FieldDef {
    std::string name;
    FfiKind     kind;
    std::size_t offset;
};

struct StructDef {
    std::vector<FieldDef> fields;
    std::size_t           size  = 0;
    std::size_t           align = 1;

    const FieldDef* find(const std::string& name) const {
        for (const auto& f : fields) if (f.name == name) return &f;
        return nullptr;
    }
};

using StructRegistry = std::shared_ptr<std::unordered_map<std::string, StructDef>>;

StructDef compute_layout(const std::vector<std::pair<std::string, FfiKind>>& fields_in) {
    StructDef out;
    std::size_t off = 0;
    for (const auto& [name, kind] : fields_in) {
        std::size_t a = align_of_kind(kind);
        std::size_t s = size_of_kind(kind);
        off = (off + a - 1) & ~(a - 1);
        out.fields.push_back({name, kind, off});
        off += s;
        if (a > out.align) out.align = a;
    }
    out.size = (off + out.align - 1) & ~(out.align - 1);
    return out;
}

// ---------- typed memory access --------------------------------------------

uintptr_t value_to_ptr(const Value& v, const char* api, bool allow_null = false) {
    if (v.is_null()) {
        if (allow_null) return 0;
        throw std::runtime_error(std::string(api) + ": pointer must not be null");
    }
    if (!v.is_number())
        throw std::runtime_error(std::string(api) + ": expected pointer (number address), got " + v.type_name());
    return static_cast<uintptr_t>(v.as_number());
}

Value read_kind_at(uintptr_t base, std::size_t offset, FfiKind kind) {
    const void* p = reinterpret_cast<const void*>(base + offset);
    switch (kind) {
        case FfiKind::I8:  { int8_t  v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::I16: { int16_t v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::I32: { int32_t v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::I64: { int64_t v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::U8:  { uint8_t  v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::U16: { uint16_t v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::U32: { uint32_t v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::U64: { uint64_t v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::F32: { float    v; std::memcpy(&v, p, sizeof v); return Value{static_cast<double>(v)}; }
        case FfiKind::F64: { double   v; std::memcpy(&v, p, sizeof v); return Value{v}; }
        case FfiKind::Ptr: {
            void* v; std::memcpy(&v, p, sizeof v);
            if (!v) return Value{};
            return Value{static_cast<double>(reinterpret_cast<uintptr_t>(v))};
        }
        case FfiKind::CStr: {
            const char* v; std::memcpy(&v, p, sizeof v);
            if (!v) return Value{};
            return Value{std::string(v)};
        }
        case FfiKind::Void:
            throw std::runtime_error("ffi.read: 'void' is not readable");
    }
    return Value{};
}

void write_kind_at(uintptr_t base, std::size_t offset, FfiKind kind, const Value& value) {
    void* p = reinterpret_cast<void*>(base + offset);
    switch (kind) {
        case FfiKind::I8:  { int8_t  v = static_cast<int8_t> (value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::I16: { int16_t v = static_cast<int16_t>(value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::I32: { int32_t v = static_cast<int32_t>(value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::I64: { int64_t v = static_cast<int64_t>(value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::U8:  { uint8_t  v = static_cast<uint8_t> (value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::U16: { uint16_t v = static_cast<uint16_t>(value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::U32: { uint32_t v = static_cast<uint32_t>(value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::U64: { uint64_t v = static_cast<uint64_t>(value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::F32: { float    v = static_cast<float>   (value.as_number()); std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::F64: { double   v = value.as_number();                        std::memcpy(p, &v, sizeof v); break; }
        case FfiKind::Ptr: {
            void* v = nullptr;
            if (!value.is_null()) v = reinterpret_cast<void*>(static_cast<uintptr_t>(value.as_number()));
            std::memcpy(p, &v, sizeof v);
            break;
        }
        case FfiKind::CStr:
            throw std::runtime_error(
                "ffi.write: writing cstr by storing a pointer is unsafe; "
                "use ffi.write_cstr(ptr, offset, str) to copy bytes into a buffer");
        case FfiKind::Void:
            throw std::runtime_error("ffi.write: 'void' is not writable");
    }
}

// ---------- per-call scratch -----------------------------------------------

// Temporaries that must outlive the ffi_call. cstr args copy the bnl string
// into one of these slots so the C side sees a valid null-terminated buffer.
struct CallScratch {
    std::vector<std::string> owned;
};

void* marshal_arg(FfiKind kind, const Value& v, void* slot, CallScratch& scratch) {
    switch (kind) {
        case FfiKind::I8:  *static_cast<int8_t*> (slot) = static_cast<int8_t> (v.as_number()); return slot;
        case FfiKind::I16: *static_cast<int16_t*>(slot) = static_cast<int16_t>(v.as_number()); return slot;
        case FfiKind::I32: *static_cast<int32_t*>(slot) = static_cast<int32_t>(v.as_number()); return slot;
        case FfiKind::I64: *static_cast<int64_t*>(slot) = static_cast<int64_t>(v.as_number()); return slot;
        case FfiKind::U8:  *static_cast<uint8_t*> (slot) = static_cast<uint8_t> (v.as_number()); return slot;
        case FfiKind::U16: *static_cast<uint16_t*>(slot) = static_cast<uint16_t>(v.as_number()); return slot;
        case FfiKind::U32: *static_cast<uint32_t*>(slot) = static_cast<uint32_t>(v.as_number()); return slot;
        case FfiKind::U64: *static_cast<uint64_t*>(slot) = static_cast<uint64_t>(v.as_number()); return slot;
        case FfiKind::F32: *static_cast<float*>  (slot) = static_cast<float> (v.as_number());  return slot;
        case FfiKind::F64: *static_cast<double*> (slot) = v.as_number();                       return slot;
        case FfiKind::Ptr: {
            void* p = nullptr;
            if (v.is_null())        p = nullptr;
            else if (v.is_number()) p = reinterpret_cast<void*>(static_cast<uintptr_t>(v.as_number()));
            else throw std::runtime_error("ffi: ptr arg must be null or a number address");
            *static_cast<void**>(slot) = p;
            return slot;
        }
        case FfiKind::CStr: {
            const char* p = nullptr;
            if (v.is_null()) {
                p = nullptr;
            } else if (v.is_string()) {
                scratch.owned.push_back(v.as_string());
                p = scratch.owned.back().c_str();
            } else {
                throw std::runtime_error("ffi: cstr arg must be a string or null");
            }
            *static_cast<const char**>(slot) = p;
            return slot;
        }
        case FfiKind::Void:
            throw std::runtime_error("ffi: 'void' is not a valid argument type");
    }
    return slot;
}

Value unmarshal_return(FfiKind kind, void* slot) {
    switch (kind) {
        case FfiKind::Void: return Value{};
        case FfiKind::I8:  return Value{static_cast<double>(*static_cast<int8_t*> (slot))};
        case FfiKind::I16: return Value{static_cast<double>(*static_cast<int16_t*>(slot))};
        case FfiKind::I32: return Value{static_cast<double>(*static_cast<int32_t*>(slot))};
        case FfiKind::I64: return Value{static_cast<double>(*static_cast<int64_t*>(slot))};
        case FfiKind::U8:  return Value{static_cast<double>(*static_cast<uint8_t*> (slot))};
        case FfiKind::U16: return Value{static_cast<double>(*static_cast<uint16_t*>(slot))};
        case FfiKind::U32: return Value{static_cast<double>(*static_cast<uint32_t*>(slot))};
        case FfiKind::U64: return Value{static_cast<double>(*static_cast<uint64_t*>(slot))};
        case FfiKind::F32: return Value{static_cast<double>(*static_cast<float*> (slot))};
        case FfiKind::F64: return Value{*static_cast<double*>(slot)};
        case FfiKind::Ptr: {
            void* p = *static_cast<void**>(slot);
            if (!p) return Value{};
            return Value{static_cast<double>(reinterpret_cast<uintptr_t>(p))};
        }
        case FfiKind::CStr: {
            const char* p = *static_cast<const char**>(slot);
            if (!p) return Value{};
            return Value{std::string(p)};
        }
    }
    return Value{};
}

// ---------- callables exposed to bnl ---------------------------------------

// Library handles ride inside Value as CallablePtrs. Calling one is an error
// — it's an opaque handle, not a function. Wrapping as Callable means we
// don't need to add a new Value variant just for this.
class FfiLibrary : public Callable {
public:
    FfiLibrary(std::string label, std::shared_ptr<DynamicLibrary> lib)
        : label_(std::move(label)), lib_(std::move(lib)) {}

    Value       call(Interpreter&, std::vector<Value>) override {
        throw std::runtime_error(
            "ffi: library handle is not callable; use ffi.fn(lib, name, ret, args) to bind a function");
    }
    int         arity() const override { return 0; }
    std::string name()  const override { return "ffi.lib(\"" + label_ + "\")"; }

    DynamicLibrary&                       dyn()     { return *lib_; }
    const std::shared_ptr<DynamicLibrary>& dyn_ptr() const { return lib_; }
    const std::string&                    label()  const { return label_; }

private:
    std::string                     label_;
    std::shared_ptr<DynamicLibrary> lib_;
};

class FfiBoundFunction : public Callable {
public:
    FfiBoundFunction(std::string label,
                     void*       fn_ptr,
                     FfiType     ret,
                     std::vector<FfiType> args,
                     std::shared_ptr<DynamicLibrary> keepalive)
        : label_(std::move(label)),
          fn_ptr_(fn_ptr),
          ret_(ret),
          args_(std::move(args)),
          keepalive_(std::move(keepalive))
    {
        raw_arg_types_.reserve(args_.size());
        for (auto& a : args_) raw_arg_types_.push_back(a.type);
        ffi_status st = ffi_prep_cif(
            &cif_, FFI_DEFAULT_ABI,
            static_cast<unsigned>(raw_arg_types_.size()),
            ret_.type,
            raw_arg_types_.empty() ? nullptr : raw_arg_types_.data());
        if (st != FFI_OK) {
            throw std::runtime_error(
                "ffi.fn '" + label_ + "': ffi_prep_cif failed (status "
                + std::to_string(int(st)) + ")");
        }
    }

    Value call(Interpreter&, std::vector<Value> values) override {
        const std::size_t n = args_.size();
        // 8-byte slot per arg covers every scalar we support (incl. 64-bit
        // pointers on every target preset). The slot array is contiguous so
        // libffi can index through it via arg_ptrs[].
        std::vector<std::uint64_t> slots(n == 0 ? 1 : n, 0);
        std::vector<void*>         arg_ptrs(n, nullptr);
        CallScratch                scratch;

        for (std::size_t i = 0; i < n; ++i) {
            arg_ptrs[i] = marshal_arg(args_[i].kind, values[i],
                                      &slots[i], scratch);
        }

        // libffi wants return storage at least sizeof(ffi_arg) bytes wide.
        // 16 bytes covers every scalar we support with headroom.
        alignas(16) std::uint64_t ret_storage[2] = {0, 0};
        ffi_call(&cif_, FFI_FN(fn_ptr_), &ret_storage, arg_ptrs.data());
        return unmarshal_return(ret_.kind, &ret_storage);
    }

    int         arity() const override { return static_cast<int>(args_.size()); }
    std::string name()  const override { return "ffi.fn(\"" + label_ + "\")"; }

private:
    std::string                     label_;
    void*                           fn_ptr_;
    FfiType                         ret_;
    std::vector<FfiType>            args_;
    std::vector<ffi_type*>          raw_arg_types_;
    ffi_cif                         cif_{};
    std::shared_ptr<DynamicLibrary> keepalive_;
};

// ---------- cross-platform library resolution ------------------------------

std::shared_ptr<DynamicLibrary> open_with_resolution(const std::string& name) {
    auto try_open = [](const std::string& p) -> std::shared_ptr<DynamicLibrary> {
        try { return std::make_shared<DynamicLibrary>(std::filesystem::path(p)); }
        catch (...) { return nullptr; }
    };

    bool has_sep = name.find('/') != std::string::npos
                || name.find('\\') != std::string::npos;
    bool has_ext = false;
    for (const char* ext : {".dll", ".so", ".dylib"}) {
        if (name.size() >= 4 && name.find(ext) != std::string::npos) {
            has_ext = true; break;
        }
    }

    std::vector<std::string> attempts = {name};
    if (!has_sep && !has_ext) {
#if defined(_WIN32)
        attempts.push_back(name + ".dll");
#elif defined(__APPLE__)
        attempts.push_back("lib" + name + ".dylib");
        attempts.push_back(name + ".dylib");
#else
        attempts.push_back("lib" + name + ".so");
        attempts.push_back("lib" + name + ".so.6");
        attempts.push_back(name + ".so");
#endif
    }

    std::string failures;
    for (const auto& p : attempts) {
        if (auto lib = try_open(p)) return lib;
        if (!failures.empty()) failures += ", ";
        failures += "'" + p + "'";
    }
    throw std::runtime_error("ffi.open: could not load any of: " + failures);
}

std::shared_ptr<DynamicLibrary> open_libc() {
    std::vector<std::string> candidates;
#if defined(_WIN32)
    candidates = {"ucrtbase.dll", "msvcrt.dll"};
#elif defined(__APPLE__)
    candidates = {"libSystem.dylib", "libc.dylib"};
#else
    candidates = {"libc.so.6", "libc.so"};
#endif
    for (const auto& p : candidates) {
        try {
            return std::make_shared<DynamicLibrary>(std::filesystem::path(p));
        } catch (...) {}
    }
    throw std::runtime_error("ffi.open_c: could not locate platform C runtime");
}

// ---------- arg helpers ----------------------------------------------------

std::shared_ptr<FfiLibrary> as_library(const Value& v, const char* api) {
    if (v.is_callable()) {
        if (auto lib = std::dynamic_pointer_cast<FfiLibrary>(v.as_callable())) return lib;
    }
    throw std::runtime_error(
        std::string(api) + ": first arg must be a library handle from ffi.open() / ffi.open_c()");
}

std::vector<FfiType> types_from_list(const Value& v, const char* api) {
    if (!v.is_list())
        throw std::runtime_error(std::string(api) + ": arg-types must be a list of type-name strings");
    std::vector<FfiType> out;
    out.reserve(v.as_list()->size());
    for (const auto& e : *v.as_list()) {
        if (!e.is_string())
            throw std::runtime_error(std::string(api) + ": each arg type must be a string");
        out.push_back(type_from_name(e.as_string()));
    }
    return out;
}

}  // namespace

void register_ffi(Interpreter& interp) {
    // Per-module struct registry. Captured by every function that touches
    // struct types so they all share the same definitions.
    auto structs = std::make_shared<std::unordered_map<std::string, StructDef>>();

    auto m = NativeModule("ffi")
        .add_function("open", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("ffi.open(name): name must be a string");
                auto lib = open_with_resolution(args[0].as_string());
                auto wrapper = std::make_shared<FfiLibrary>(args[0].as_string(), std::move(lib));
                return Value{std::static_pointer_cast<Callable>(wrapper)};
            })

        .add_function("open_c", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                auto lib = open_libc();
                auto wrapper = std::make_shared<FfiLibrary>("c", std::move(lib));
                return Value{std::static_pointer_cast<Callable>(wrapper)};
            })

        // No-op-ish: refcount drops when the user releases their handle. We
        // accept the call for symmetry / explicit intent.
        .add_function("close", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                as_library(args[0], "ffi.close");
                return Value{};
            })

        .add_function("fn", 4,
            [](Interpreter&, std::vector<Value> args) -> Value {
                auto lib = as_library(args[0], "ffi.fn");
                if (!args[1].is_string())
                    throw std::runtime_error("ffi.fn: symbol name must be a string");
                if (!args[2].is_string())
                    throw std::runtime_error("ffi.fn: return type must be a string");
                FfiType              ret_type  = type_from_name(args[2].as_string());
                std::vector<FfiType> arg_types = types_from_list(args[3], "ffi.fn");

                void* fn_ptr = lib->dyn().symbol(args[1].as_string().c_str());
                if (!fn_ptr) {
                    throw std::runtime_error(
                        "ffi.fn: symbol '" + args[1].as_string()
                        + "' not found in " + lib->label());
                }
                auto bound = std::make_shared<FfiBoundFunction>(
                    args[1].as_string(),
                    fn_ptr, ret_type, std::move(arg_types),
                    lib->dyn_ptr());
                return Value{std::static_pointer_cast<Callable>(bound)};
            })

        // ---- memory ---------------------------------------------------------

        // ffi.alloc(size) — malloc `size` bytes; returns the address as a
        // pointer. Memory is zero-initialized for predictability. Must be
        // freed with ffi.free() — pointers from ffi.alloc are NOT
        // interchangeable with foreign malloc'd pointers (different CRTs).
        .add_function("alloc", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_number())
                    throw std::runtime_error("ffi.alloc(size): size must be a number");
                auto n = static_cast<std::size_t>(args[0].as_number());
                void* p = std::calloc(1, n == 0 ? 1 : n);  // calloc clears + handles n==0 safely
                if (!p) throw std::runtime_error("ffi.alloc: out of memory");
                return Value{static_cast<double>(reinterpret_cast<uintptr_t>(p))};
            })

        // ffi.free(ptr) — releases memory from ffi.alloc / ffi.alloc_struct.
        // null is a no-op (matches free(NULL) semantics).
        .add_function("free", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args[0].is_null()) return Value{};
                uintptr_t addr = value_to_ptr(args[0], "ffi.free");
                std::free(reinterpret_cast<void*>(addr));
                return Value{};
            })

        // ---- typed scalar memory access -------------------------------------

        // ffi.read(ptr, type, offset) — read one value of `type` at `offset`
        // from `ptr`. Returns the value as a bnl number / string / null.
        .add_function("read", 3,
            [](Interpreter&, std::vector<Value> args) -> Value {
                uintptr_t base = value_to_ptr(args[0], "ffi.read");
                if (!args[1].is_string())
                    throw std::runtime_error("ffi.read: type must be a string");
                if (!args[2].is_number())
                    throw std::runtime_error("ffi.read: offset must be a number");
                FfiType t = type_from_name(args[1].as_string());
                auto off = static_cast<std::size_t>(args[2].as_number());
                return read_kind_at(base, off, t.kind);
            })

        // ffi.write(ptr, type, offset, value) — write `value` as `type` at
        // `offset` from `ptr`. For cstr use ffi.write_cstr(ptr, offset, str).
        .add_function("write", 4,
            [](Interpreter&, std::vector<Value> args) -> Value {
                uintptr_t base = value_to_ptr(args[0], "ffi.write");
                if (!args[1].is_string())
                    throw std::runtime_error("ffi.write: type must be a string");
                if (!args[2].is_number())
                    throw std::runtime_error("ffi.write: offset must be a number");
                FfiType t = type_from_name(args[1].as_string());
                auto off = static_cast<std::size_t>(args[2].as_number());
                write_kind_at(base, off, t.kind, args[3]);
                return Value{};
            })

        // ffi.read_cstr(ptr) — read a null-terminated UTF-8 C string from
        // `ptr`. Returns null if ptr is null.
        .add_function("read_cstr", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args[0].is_null()) return Value{};
                uintptr_t addr = value_to_ptr(args[0], "ffi.read_cstr");
                return Value{std::string(reinterpret_cast<const char*>(addr))};
            })

        // ffi.write_cstr(ptr, offset, str) — copies the UTF-8 bytes of `str`
        // plus a trailing NUL into the buffer at `ptr + offset`. Returns the
        // number of bytes written (str.length + 1). Caller must ensure the
        // buffer is large enough.
        .add_function("write_cstr", 3,
            [](Interpreter&, std::vector<Value> args) -> Value {
                uintptr_t base = value_to_ptr(args[0], "ffi.write_cstr");
                if (!args[1].is_number())
                    throw std::runtime_error("ffi.write_cstr: offset must be a number");
                if (!args[2].is_string())
                    throw std::runtime_error("ffi.write_cstr: value must be a string");
                auto off = static_cast<std::size_t>(args[1].as_number());
                const auto& s = args[2].as_string();
                char* dst = reinterpret_cast<char*>(base + off);
                std::memcpy(dst, s.data(), s.size());
                dst[s.size()] = '\0';
                return Value{static_cast<double>(s.size() + 1)};
            })

        // ---- struct support -------------------------------------------------

        // ffi.struct(name, fields_map) — declare a struct layout. `fields_map`
        // is a bnl map {field_name: type_name}. Layout follows the platform's
        // default C ABI alignment rules (no packing, no bitfields). Re-
        // declaring a name overwrites the prior definition.
        .add_function("struct", 2,
            [structs](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("ffi.struct(name, fields): name must be a string");
                if (!args[1].is_map())
                    throw std::runtime_error("ffi.struct(name, fields): fields must be a map");
                std::vector<std::pair<std::string, FfiKind>> fields;
                for (const auto& [fname, ftype] : *args[1].as_map()) {
                    if (!ftype.is_string())
                        throw std::runtime_error(
                            "ffi.struct: field '" + fname + "' type must be a string");
                    FfiType t;
                    if (!try_type_from_name(ftype.as_string(), &t))
                        throw std::runtime_error(
                            "ffi.struct: field '" + fname + "' has unknown type '"
                            + ftype.as_string() + "'");
                    if (t.kind == FfiKind::Void)
                        throw std::runtime_error(
                            "ffi.struct: field '" + fname + "' cannot be 'void'");
                    fields.push_back({fname, t.kind});
                }
                (*structs)[args[0].as_string()] = compute_layout(fields);
                return Value{};
            })

        // ffi.size_of(name) — bytes occupied by `name`. Works for primitives
        // ("int32", "ptr", ...) and for previously-declared struct names.
        .add_function("size_of", 1,
            [structs](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("ffi.size_of: name must be a string");
                const auto& name = args[0].as_string();
                FfiType t;
                if (try_type_from_name(name, &t))
                    return Value{static_cast<double>(size_of_kind(t.kind))};
                auto it = structs->find(name);
                if (it == structs->end())
                    throw std::runtime_error("ffi.size_of: unknown type or struct '" + name + "'");
                return Value{static_cast<double>(it->second.size)};
            })

        // ffi.offset_of(struct_name, field_name) — byte offset of a field
        // within its struct. Useful for debugging or manual layouts.
        .add_function("offset_of", 2,
            [structs](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string() || !args[1].is_string())
                    throw std::runtime_error("ffi.offset_of(struct, field): both args must be strings");
                auto it = structs->find(args[0].as_string());
                if (it == structs->end())
                    throw std::runtime_error("ffi.offset_of: unknown struct '" + args[0].as_string() + "'");
                const FieldDef* f = it->second.find(args[1].as_string());
                if (!f) throw std::runtime_error(
                    "ffi.offset_of: struct '" + args[0].as_string()
                    + "' has no field '" + args[1].as_string() + "'");
                return Value{static_cast<double>(f->offset)};
            })

        // ffi.alloc_struct(name) — allocate sizeof(struct) zero-initialized
        // bytes and return the pointer.
        .add_function("alloc_struct", 1,
            [structs](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("ffi.alloc_struct: name must be a string");
                auto it = structs->find(args[0].as_string());
                if (it == structs->end())
                    throw std::runtime_error("ffi.alloc_struct: unknown struct '" + args[0].as_string() + "'");
                void* p = std::calloc(1, it->second.size == 0 ? 1 : it->second.size);
                if (!p) throw std::runtime_error("ffi.alloc_struct: out of memory");
                return Value{static_cast<double>(reinterpret_cast<uintptr_t>(p))};
            })

        // ffi.field(ptr, struct_name, field_name) — read a struct field.
        .add_function("field", 3,
            [structs](Interpreter&, std::vector<Value> args) -> Value {
                uintptr_t base = value_to_ptr(args[0], "ffi.field");
                if (!args[1].is_string() || !args[2].is_string())
                    throw std::runtime_error("ffi.field(ptr, struct, field): struct and field must be strings");
                auto it = structs->find(args[1].as_string());
                if (it == structs->end())
                    throw std::runtime_error("ffi.field: unknown struct '" + args[1].as_string() + "'");
                const FieldDef* f = it->second.find(args[2].as_string());
                if (!f) throw std::runtime_error(
                    "ffi.field: struct '" + args[1].as_string()
                    + "' has no field '" + args[2].as_string() + "'");
                return read_kind_at(base, f->offset, f->kind);
            })

        // ffi.set_field(ptr, struct_name, field_name, value) — write a field.
        .add_function("set_field", 4,
            [structs](Interpreter&, std::vector<Value> args) -> Value {
                uintptr_t base = value_to_ptr(args[0], "ffi.set_field");
                if (!args[1].is_string() || !args[2].is_string())
                    throw std::runtime_error("ffi.set_field(ptr, struct, field, value): struct and field must be strings");
                auto it = structs->find(args[1].as_string());
                if (it == structs->end())
                    throw std::runtime_error("ffi.set_field: unknown struct '" + args[1].as_string() + "'");
                const FieldDef* f = it->second.find(args[2].as_string());
                if (!f) throw std::runtime_error(
                    "ffi.set_field: struct '" + args[1].as_string()
                    + "' has no field '" + args[2].as_string() + "'");
                write_kind_at(base, f->offset, f->kind, args[3]);
                return Value{};
            })

        .build();

    interp.register_native_module("ffi", m);
}

}  // namespace bnl
