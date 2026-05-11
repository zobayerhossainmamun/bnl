#pragma once

#include <filesystem>
#include <string>

namespace bnl {

// RAII handle around LoadLibrary / dlopen. Throws std::runtime_error on
// failure with a per-OS error message attached.
class DynamicLibrary {
public:
    explicit DynamicLibrary(const std::filesystem::path& path);
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&)            = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&&) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&&) noexcept;

    // Returns NULL if the symbol is not exported.
    void* symbol(const char* name);

    const std::filesystem::path& path() const { return path_; }

private:
    void*                  handle_;
    std::filesystem::path  path_;
};

}  // namespace bnl
