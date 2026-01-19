#pragma once

#include <string>
#include <filesystem>

inline std::string u8Path(std::filesystem::path const& path)
{
    const auto u8Str = path.generic_u8string();
    return std::string(u8Str.begin(), u8Str.end());
}