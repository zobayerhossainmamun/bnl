#pragma once

#include <filesystem>
#include <string>

namespace bnl {

// Thin RAII wrapper around LoadLibraryW (Windows) / dlopen (POSIX).
// Internal — used by ModuleLoader to back native FFI plugins.
class DynamicLibrary {
public:
    // Loads the shared library at `path`. Throws std::runtime_error on failure
    // (callers convert to ModuleError to surface line/col).
    explicit DynamicLibrary(const std::filesystem::path& path);
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&)            = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&&)      = delete;

    // Returns nullptr if the symbol is not exported.
    void* symbol(const char* name) const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    void*                 handle_ = nullptr;
};

}  // namespace bnl
