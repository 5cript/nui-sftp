#pragma once

#include <string>
#include <string_view>
#include <filesystem>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif

/**
 * @brief Converts a path to a UTF-8 string (lossless on Windows).
 * @param path The host-native path.
 */
inline std::string u8Path(std::filesystem::path const& path)
{
    const auto u8Str = path.generic_u8string();
    return std::string(u8Str.begin(), u8Str.end());
}

/**
 * @brief Constructs a std::filesystem::path from a UTF-8 string.
 * @param utf8 The UTF-8 encoded input (as received from libssh or RPC).
 *
 * On Windows this transcodes UTF-8 to UTF-16 before constructing the path,
 * so non-ASCII bytes are preserved. On POSIX paths are already UTF-8.
 */
inline std::filesystem::path pathFromU8(std::string_view utf8)
{
#ifdef _WIN32
    if (utf8.empty())
        return std::filesystem::path{};
    const int sizeNeeded = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0
    );
    if (sizeNeeded <= 0)
        return std::filesystem::path{std::string{utf8}};
    std::wstring wide(static_cast<std::size_t>(sizeNeeded), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), sizeNeeded
    );
    return std::filesystem::path{wide};
#else
    return std::filesystem::path{std::string{utf8}};
#endif
}
