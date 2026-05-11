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
    handle_ = LoadLibraryW(path.wstring().c_str());
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
