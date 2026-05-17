#include "ffi/dynamic_library.h"

#include <stdexcept>
#include <utility>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace bnl {

DynamicLibrary::DynamicLibrary(const std::filesystem::path& path)
    : handle_(nullptr), path_(path) {
#ifdef _WIN32
    // LOAD_WITH_ALTERED_SEARCH_PATH makes Windows resolve THIS DLL's own
    // dependencies starting from the DLL's own directory, instead of the
    // host EXE's directory. Without it, a plugin that bundles e.g. its own
    // onnxruntime.dll next to itself would be ignored in favour of an
    // older copy sitting in System32 or PATH — same fix libuv uses for
    // Node addons and Python uses (via LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
    // for .pyd extensions.
    handle_ = LoadLibraryExW(path.wstring().c_str(), nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!handle_) {
        DWORD code = GetLastError();
        throw std::runtime_error("LoadLibrary failed for "
            + path.string() + " (error " + std::to_string(code) + ")");
    }
#else
    // RTLD_NOW so missing-symbol errors surface at load, not at first call.
    // RTLD_LOCAL so plugin internals don't pollute the global symbol table.
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* err = dlerror();
        throw std::runtime_error("dlopen failed for "
            + path.string() + ": " + (err ? err : "unknown"));
    }
#endif
}

DynamicLibrary::~DynamicLibrary() {
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
    other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        if (handle_) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle_));
#else
            dlclose(handle_);
#endif
        }
        handle_      = other.handle_;
        path_        = std::move(other.path_);
        other.handle_ = nullptr;
    }
    return *this;
}

void* DynamicLibrary::symbol(const char* name) {
    if (!handle_) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

}  // namespace bnl
