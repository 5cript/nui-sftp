#pragma once

#include <paths/path_error.hpp>
#include <paths/utf8.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace Paths
{
    /**
     * @brief Native text converted to UTF-8, and whether that was lossless.
     */
    struct NativeText
    {
        /**
         * @brief The text as generic ('/' separated) UTF-8, always valid: U+FFFD stands for every byte that is not
         *        UTF-8 (POSIX) and every unpaired surrogate (Windows).
         */
        std::string utf8{};
        /**
         * @brief False when the native text had to be replaced in places, then @ref utf8 only shows it.
         */
        bool lossless{true};
    };

    /**
     * @brief The text of a native path as generic UTF-8 and whether it converted losslessly. Never throws.
     */
    inline NativeText convertNativeToUtf8(std::filesystem::path const& path)
    {
#ifdef _WIN32
        const auto wide = path.generic_wstring();
        return NativeText{
            .utf8 = utf16ToUtf8(std::wstring_view{wide}),
            .lossless = isValidUtf16(std::wstring_view{wide}),
        };
#else
        auto const& bytes = path.native();
        return NativeText{.utf8 = sanitizedUtf8(bytes), .lossless = isValidUtf8(bytes)};
#endif
    }

    /**
     * @brief The text of a native path as generic UTF-8, for display and logs: what is not UTF-8 becomes U+FFFD.
     *        A name or path that is acted on goes through @ref checkedNativeToUtf8 or LocalPath::make.
     */
    inline std::string nativeToUtf8(std::filesystem::path const& path)
    {
        return convertNativeToUtf8(path).utf8;
    }

    /**
     * @brief The text of a native path as generic UTF-8, refused with InvalidUtf8 when it does not convert
     *        losslessly (the error input shows it with U+FFFD).
     */
    inline std::expected<std::string, PathError> checkedNativeToUtf8(std::filesystem::path const& path)
    {
        auto converted = convertNativeToUtf8(path);
        if (!converted.lossless)
            return std::unexpected(PathError{.reason = PathRejection::InvalidUtf8, .input = std::move(converted.utf8)});
        return std::move(converted.utf8);
    }

    /**
     * @brief A native path from UTF-8 text, the reverse of @ref nativeToUtf8. Never throws: on Windows every byte
     *        that is not UTF-8 becomes U+FFFD, so text from a server is checked with isValidUtf8 first. An absolute
     *        path goes through LocalPath::fromUtf8, which validates it.
     */
    inline std::filesystem::path nativeFromUtf8(std::string_view utf8)
    {
#ifdef _WIN32
        return std::filesystem::path{utf8ToWide(utf8)};
#else
        return std::filesystem::path{std::string{utf8}};
#endif
    }
}
#endif
