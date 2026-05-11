#include "ffi/plugin_api.h"

#include <fmt/core.h>

#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"
#include "bnl/plugin.h"
#include "bnl/value.h"

namespace bnl {

namespace {

// ----------------------------------------------------------------------------
// Bridge model.
//
// `bnl_value*` is a pointer to a `PluginValue` (heap-allocated wrapper around
// a real bnl::Value). Lifetimes are arena-scoped: each plugin call (bnl_load
// or a native_fn) pushes an Arena onto a thread-local stack; all values
// allocated by api->make_* during the call live in that arena and are freed
// when the arena pops. Values attached to lists/maps are deep-copied at
// attach time, so they survive past the call regardless of the arena.
//
// `bnl_module*` is a PluginModule that accumulates (name, callable/value)
// entries; after bnl_load returns we feed them into a NativeModule builder
// and produce a real bnl::ModulePtr.
//
// Errors raised via api->throw_error are stashed in the current arena and
// re-thrown as std::runtime_error after the call returns.
// ----------------------------------------------------------------------------

struct PluginValue {
    Value v;
};

struct PluginModule {
    std::string name;
    std::vector<std::pair<std::string, Value>>          values;
    std::vector<std::tuple<std::string, int, NativeFunction::Fn>> functions;
};

struct Arena {
    std::vector<std::unique_ptr<PluginValue>> values;
    bool        error_set = false;
    std::string error_msg;
};

thread_local std::vector<std::unique_ptr<Arena>> g_arenas;
thread_local Interpreter*                        g_interp = nullptr;

Arena* current_arena_ptr() {
    return g_arenas.empty() ? nullptr : g_arenas.back().get();
}

PluginValue* push_value(Value v) {
    auto* a = current_arena_ptr();
    if (!a) {
        // Should be unreachable — guarded at the call sites. Returning NULL
        // would crash the plugin; better to surface the bug now.
        throw std::runtime_error(
            "bnl plugin api invoked outside a plugin context");
    }
    auto holder = std::make_unique<PluginValue>(PluginValue{std::move(v)});
    PluginValue* raw = holder.get();
    a->values.push_back(std::move(holder));
    return raw;
}

inline const PluginValue* pv(const bnl_value* p) {
    return reinterpret_cast<const PluginValue*>(p);
}
inline PluginValue* pv(bnl_value* p) {
    return reinterpret_cast<PluginValue*>(p);
}
inline bnl_value* hv(PluginValue* p) {
    return reinterpret_cast<bnl_value*>(p);
}
inline PluginModule* pm(bnl_module* m) {
    return reinterpret_cast<PluginModule*>(m);
}

// ---- api table function impls ----------------------------------------------

bnl_value* api_make_null(const bnl_api*) {
    return hv(push_value(Value{}));
}
bnl_value* api_make_bool(const bnl_api*, int b) {
    return hv(push_value(Value{b != 0}));
}
bnl_value* api_make_number(const bnl_api*, double n) {
    return hv(push_value(Value{n}));
}
bnl_value* api_make_string(const bnl_api*, const char* s, size_t len) {
    return hv(push_value(Value{std::string(s ? s : "", len)}));
}
bnl_value* api_make_list(const bnl_api*) {
    return hv(push_value(Value{std::make_shared<std::vector<Value>>()}));
}
bnl_value* api_make_map(const bnl_api*) {
    return hv(push_value(
        Value{std::make_shared<std::unordered_map<std::string, Value>>()}));
}

int api_get_type(const bnl_value* v) {
    if (!v) return BNL_TYPE_NULL;
    const auto& val = pv(v)->v;
    if (val.is_null())   return BNL_TYPE_NULL;
    if (val.is_bool())   return BNL_TYPE_BOOL;
    if (val.is_number()) return BNL_TYPE_NUMBER;
    if (val.is_string()) return BNL_TYPE_STRING;
    if (val.is_list())   return BNL_TYPE_LIST;
    if (val.is_map())    return BNL_TYPE_MAP;
    return BNL_TYPE_OTHER;
}

int api_get_bool(const bnl_value* v) {
    if (!v || !pv(v)->v.is_bool()) return 0;
    return pv(v)->v.as_bool() ? 1 : 0;
}
double api_get_number(const bnl_value* v) {
    if (!v || !pv(v)->v.is_number()) return 0.0;
    return pv(v)->v.as_number();
}
const char* api_get_string(const bnl_value* v, size_t* out_len) {
    if (!v || !pv(v)->v.is_string()) {
        if (out_len) *out_len = 0;
        return "";
    }
    const std::string& s = pv(v)->v.as_string();
    if (out_len) *out_len = s.size();
    return s.data();
}

size_t api_list_length(const bnl_value* list) {
    if (!list || !pv(list)->v.is_list()) return 0;
    return pv(list)->v.as_list()->size();
}
bnl_value* api_list_get(const bnl_api*, bnl_value* list, size_t index) {
    if (!list || !pv(list)->v.is_list()) return nullptr;
    const auto& xs = *pv(list)->v.as_list();
    if (index >= xs.size()) return nullptr;
    return hv(push_value(xs[index]));
}
void api_list_push(bnl_value* list, bnl_value* item) {
    if (!list || !item || !pv(list)->v.is_list()) return;
    auto& xs = *pv(list)->v.as_list();
    xs.push_back(pv(item)->v);
}

size_t api_map_size(const bnl_value* map) {
    if (!map || !pv(map)->v.is_map()) return 0;
    return pv(map)->v.as_map()->size();
}
int api_map_has(const bnl_value* map, const char* key) {
    if (!map || !key || !pv(map)->v.is_map()) return 0;
    return pv(map)->v.as_map()->count(key) ? 1 : 0;
}
bnl_value* api_map_get(const bnl_api*, bnl_value* map, const char* key) {
    if (!map || !key || !pv(map)->v.is_map()) return nullptr;
    const auto& m = *pv(map)->v.as_map();
    auto it = m.find(key);
    if (it == m.end()) return nullptr;
    return hv(push_value(it->second));
}
void api_map_set(bnl_value* map, const char* key, bnl_value* val) {
    if (!map || !key || !val || !pv(map)->v.is_map()) return;
    auto& m = *pv(map)->v.as_map();
    m[key] = pv(val)->v;
}
int api_map_key_at(const bnl_value* map, size_t index,
                   const char** out_key, size_t* out_len) {
    if (!map || !pv(map)->v.is_map()) return 0;
    const auto& m = *pv(map)->v.as_map();
    if (index >= m.size()) return 0;
    auto it = m.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));
    if (out_key) *out_key = it->first.data();
    if (out_len) *out_len = it->first.size();
    return 1;
}

// Build a NativeFunction::Fn that:
//   1. pushes an arena
//   2. snapshots argv into arena-allocated PluginValues
//   3. invokes the plugin's C function pointer
//   4. extracts the return Value before popping the arena
//   5. propagates throw_error as a thrown std::runtime_error
NativeFunction::Fn make_native_thunk(bnl_native_fn fn, void* userdata,
                                      const bnl_api* api_ptr,
                                      std::string display_name) {
    return [fn, userdata, api_ptr, name = std::move(display_name)]
           (Interpreter&, std::vector<Value> args) -> Value {
        g_arenas.push_back(std::make_unique<Arena>());

        std::vector<bnl_value*> argv;
        argv.reserve(args.size());
        for (auto& a : args) {
            argv.push_back(hv(push_value(std::move(a))));
        }

        bnl_value* ret = nullptr;
        try {
            ret = fn(api_ptr,
                     static_cast<int>(argv.size()),
                     argv.empty() ? nullptr : argv.data(),
                     userdata);
        } catch (...) {
            // A C plugin shouldn't throw, but if it does (e.g. C++ plugin
            // using the C API), don't leak the arena.
            g_arenas.pop_back();
            throw;
        }

        Value       out;
        bool        was_error = g_arenas.back()->error_set;
        std::string err_msg   = std::move(g_arenas.back()->error_msg);
        if (ret) out = pv(ret)->v;
        g_arenas.pop_back();

        if (was_error) {
            throw std::runtime_error(
                "plugin function '" + name + "': "
                + (err_msg.empty() ? "unspecified error" : err_msg));
        }
        return out;
    };
}

// Forward decl: api_table() returns the singleton api with this fn baked in.
const bnl_api& api_table();

bnl_module* api_module_new(const bnl_api*, const char* name) {
    auto* m = new PluginModule;
    m->name = name ? name : "";
    return reinterpret_cast<bnl_module*>(m);
}
void api_module_add_function(bnl_module* mod, const char* name, int arity,
                             bnl_native_fn fn, void* userdata) {
    if (!mod || !name || !fn) return;
    pm(mod)->functions.emplace_back(
        std::string{name}, arity,
        make_native_thunk(fn, userdata, &api_table(), name));
}
void api_module_add_value(bnl_module* mod, const char* name, bnl_value* value) {
    if (!mod || !name) return;
    Value v = value ? pv(value)->v : Value{};
    pm(mod)->values.emplace_back(std::string{name}, std::move(v));
}

void api_throw_error(const bnl_api*, const char* msg) {
    auto* a = current_arena_ptr();
    if (!a) return;
    a->error_set = true;
    a->error_msg = msg ? msg : "";
}

const bnl_api& api_table() {
    static const bnl_api table = {
        BNL_PLUGIN_API_VERSION,

        api_make_null, api_make_bool, api_make_number,
        api_make_string, api_make_list, api_make_map,

        api_get_type, api_get_bool, api_get_number, api_get_string,

        api_list_length, api_list_get, api_list_push,

        api_map_size, api_map_has, api_map_get, api_map_set, api_map_key_at,

        api_module_new, api_module_add_function, api_module_add_value,

        api_throw_error,
    };
    return table;
}

}  // namespace

ModulePtr load_c_plugin_module(DynamicLibrary&              lib,
                                const std::filesystem::path& path,
                                Interpreter&                 interp) {
    auto entry = reinterpret_cast<bnl_load_fn>(lib.symbol("bnl_load"));
    if (!entry) {
        throw std::runtime_error(fmt::format(
            "plugin '{}' has no 'bnl_load' export — see <bnl/plugin.h>",
            path.string()));
    }

    g_arenas.push_back(std::make_unique<Arena>());
    g_interp = &interp;

    bnl_module* raw_module = nullptr;
    try {
        raw_module = entry(&api_table());
    } catch (...) {
        g_arenas.pop_back();
        g_interp = nullptr;
        throw;
    }

    bool        was_error = g_arenas.back()->error_set;
    std::string err_msg   = std::move(g_arenas.back()->error_msg);
    g_arenas.pop_back();
    g_interp = nullptr;

    std::unique_ptr<PluginModule> pm_holder{pm(raw_module)};

    if (was_error) {
        throw std::runtime_error(fmt::format(
            "plugin '{}' bnl_load: {}",
            path.string(),
            err_msg.empty() ? "unspecified error" : err_msg));
    }
    if (!pm_holder) {
        throw std::runtime_error(fmt::format(
            "plugin '{}' bnl_load returned NULL", path.string()));
    }

    auto builder = NativeModule(pm_holder->name);
    for (auto& [name, arity, fn] : pm_holder->functions) {
        builder.add_function(std::move(name), arity, std::move(fn));
    }
    for (auto& [name, val] : pm_holder->values) {
        builder.add_value(std::move(name), std::move(val));
    }
    return builder.build();
}

}  // namespace bnl
