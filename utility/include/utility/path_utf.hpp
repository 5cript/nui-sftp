#pragma once

#include <nui/utility/utf.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace Utility
{
    /**
     * @brief Constructs a std::filesystem::path from a UTF-8 encoded string.
     * @param utf8 The UTF-8 encoded input.
     *
     * Analogous to ntoh*: converts from the wire/portable encoding (UTF-8, as
     * used over RPC and by Emscripten) to the host's native path encoding.
     * On Windows the native encoding is UTF-16, so the input is transcoded;
     * on POSIX paths are already UTF-8.
     */
    inline std::filesystem::path pathFromUtf8(std::string_view utf8)
    {
#ifdef _WIN32
        const std::string owned{utf8};
        const auto utf16 = Nui::utf8ToUtf16<std::wstring>(owned);
        return std::filesystem::path{utf16};
#else
        return std::filesystem::path{std::string{utf8}};
#endif
    }

    /**
     * @brief Converts a std::filesystem::path to a UTF-8 encoded string.
     * @param path The path in host-native encoding.
     *
     * Analogous to hton*: converts from the host's native path encoding to
     * the wire/portable encoding (UTF-8) used by RPC and Emscripten.
     */
    inline std::string pathToUtf8(std::filesystem::path const& path)
    {
#ifdef _WIN32
        return Nui::utf16ToUtf8<std::wstring, std::string>(path.native());
#else
        return path.native();
#endif
    }

    /**
     * @brief Like pathToUtf8 but using the generic (forward-slash) form.
     * @param path The path in host-native encoding.
     */
    inline std::string pathToUtf8Generic(std::filesystem::path const& path)
    {
#ifdef _WIN32
        return Nui::utf16ToUtf8<std::wstring, std::string>(path.generic_wstring());
#else
        return path.generic_string();
#endif
    }
}
