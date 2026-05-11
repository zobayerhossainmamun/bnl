#pragma once

namespace bnl {

inline constexpr const char* kVersion = "1.0.0";

#if defined(_WIN32)
inline constexpr const char* kPlatform = "windows";
#elif defined(__APPLE__)
inline constexpr const char* kPlatform = "macos";
#elif defined(__linux__)
inline constexpr const char* kPlatform = "linux";
#else
inline constexpr const char* kPlatform = "unknown";
#endif

}  // namespace bnl
