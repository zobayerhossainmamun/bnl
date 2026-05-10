#include "ffi/dynamic_library.h"

#include <fmt/core.h>

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
    : path_(path) {
#ifdef _WIN32
    // LOAD_WITH_ALTERED_SEARCH_PATH makes the plugin's own directory the
    // first place Windows looks for its sibling dependencies (e.g. a
    // plugin that links sqlite3.dll keeps sqlite3.dll next to itself).
    // Without this, Windows uses bnl.exe's directory and reports
    // "module not found" for any sibling DLL that's not staged there.
    handle_ = LoadLibraryExW(path.wstring().c_str(), nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!handle_) {
        DWORD err = GetLastError();
        throw std::runtime_error(fmt::format(
            "failed to load native plugin '{}' (Windows error {})",
            path.string(), err));
    }
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* err = dlerror();
        throw std::runtime_error(fmt::format(
            "failed to load native plugin '{}': {}",
            path.string(), err ? err : "unknown error"));
    }
#endif
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : path_(std::move(other.path_)), handle_(other.handle_) {
    other.handle_ = nullptr;
}

DynamicLibrary::~DynamicLibrary() {
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
}

void* DynamicLibrary::symbol(const char* name) const {
    if (!handle_) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

}  // namespace bnl
