#pragma once

#include <paths/path_error.hpp>

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace Paths
{
    /**
     * @brief True when @p text is well-formed UTF-8 (no overlong forms, no surrogates).
     */
    inline bool isValidUtf8(std::string_view text)
    {
        std::size_t index = 0;
        while (index < text.size())
        {
            const auto lead = static_cast<unsigned char>(text[index]);
            std::size_t length = 0;
            std::uint32_t codePoint = 0;
            if (lead < 0x80u)
            {
                ++index;
                continue;
            }
            if ((lead & 0xE0u) == 0xC0u)
            {
                length = 2;
                codePoint = lead & 0x1Fu;
            }
            else if ((lead & 0xF0u) == 0xE0u)
            {
                length = 3;
                codePoint = lead & 0x0Fu;
            }
            else if ((lead & 0xF8u) == 0xF0u)
            {
                length = 4;
                codePoint = lead & 0x07u;
            }
            else
                return false;

            if (index + length > text.size())
                return false;
            for (std::size_t offset = 1; offset < length; ++offset)
            {
                const auto continuation = static_cast<unsigned char>(text[index + offset]);
                if ((continuation & 0xC0u) != 0x80u)
                    return false;
                codePoint = (codePoint << 6u) | (continuation & 0x3Fu);
            }
            constexpr std::uint32_t minimumForLength[] = {0u, 0u, 0x80u, 0x800u, 0x10000u};
            if (codePoint < minimumForLength[length] || codePoint > 0x10FFFFu ||
                (codePoint >= 0xD800u && codePoint <= 0xDFFFu))
                return false;
            index += length;
        }
        return true;
    }

    /**
     * @brief One name of a path: never empty, "." or "..", never containing '/' or NUL, always UTF-8.
     *
     * A remote name with a backslash or colon is a valid segment, whether it can exist locally is decided by
     * @ref LocalPathSegment.
     */
    class PathSegment
    {
      public:
        static std::expected<PathSegment, PathError> make(std::string_view utf8)
        {
            const auto refuse = [utf8](PathRejection reason)
            {
                return std::unexpected(PathError{.reason = reason, .input = std::string{utf8}});
            };
            if (utf8.empty())
                return refuse(PathRejection::Empty);
            if (utf8 == "." || utf8 == "..")
                return refuse(PathRejection::DotOrDotDot);
            if (utf8.find('/') != std::string_view::npos)
                return refuse(PathRejection::ContainsSeparator);
            if (utf8.find('\0') != std::string_view::npos)
                return refuse(PathRejection::ContainsNul);
            if (!isValidUtf8(utf8))
                return refuse(PathRejection::InvalidUtf8);
            return PathSegment{std::string{utf8}};
        }

        std::string const& value() const noexcept
        {
            return value_;
        }

        std::string_view view() const noexcept
        {
            return value_;
        }

        auto operator<=>(PathSegment const&) const = default;

      private:
        explicit PathSegment(std::string value)
            : value_{std::move(value)}
        {}

        std::string value_;
    };
}
