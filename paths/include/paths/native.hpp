#pragma once

#ifndef __EMSCRIPTEN__
#    include <filesystem>
#    include <string>
#    include <string_view>

namespace Paths
{
    /**
     * @brief The text of a native path as generic UTF-8. For names and link literals, which are no LocalPath; an
     *        absolute path goes through LocalPath::make.
     */
    inline std::string nativeToUtf8(std::filesystem::path const& path)
    {
        const auto text = path.generic_u8string();
        return std::string{reinterpret_cast<char const*>(text.data()), text.size()};
    }

    /**
     * @brief A native path from UTF-8 text, the reverse of @ref nativeToUtf8. An absolute path goes through
     *        LocalPath::fromUtf8, which validates it.
     */
    inline std::filesystem::path nativeFromUtf8(std::string_view utf8)
    {
        return std::filesystem::path{std::u8string_view{reinterpret_cast<char8_t const*>(utf8.data()), utf8.size()}};
    }
}
#endif
