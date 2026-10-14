#pragma once

#include <paths/path_error.hpp>
#include <paths/utf8.hpp>

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace Paths
{
    class RelativePath;
    class RemotePath;
    class LocalPath;

    namespace Detail
    {
        /**
         * @brief The parts of @p text between '/' separators as string views, empty parts included. Empty text has
         *        no parts.
         */
        inline auto splitAtSlashes(std::string_view text)
        {
            return text | std::views::split('/') |
                std::views::transform(
                       [](auto const& part)
                       {
                           return std::string_view{part.begin(), part.end()};
                       }
                );
        }

        /**
         * @brief Appends @p tail to the path text @p value, with a '/' between them unless @p value ends in one.
         */
        inline void appendSegment(std::string& value, std::string_view tail)
        {
            if (!value.empty() && value.back() != '/')
                value.push_back('/');
            value.append(tail);
        }

        /**
         * @brief The text after the last '/' of @p value, all of it when there is none.
         */
        inline std::string_view lastSegment(std::string_view value)
        {
            const auto slash = value.rfind('/');
            return slash == std::string_view::npos ? value : value.substr(slash + 1);
        }
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
            if (const auto rejection = rejectionOf(utf8); rejection)
                return std::unexpected(PathError{.reason = *rejection, .input = std::string{utf8}});
            return PathSegment{std::string{utf8}};
        }

        /**
         * @brief Why @p utf8 is no valid segment, nothing when it is one.
         */
        static std::optional<PathRejection> rejectionOf(std::string_view utf8)
        {
            if (utf8.empty())
                return PathRejection::Empty;
            if (utf8 == "." || utf8 == "..")
                return PathRejection::DotOrDotDot;
            if (utf8.find('/') != std::string_view::npos)
                return PathRejection::ContainsSeparator;
            if (utf8.find('\0') != std::string_view::npos)
                return PathRejection::ContainsNul;
            if (!isValidUtf8(utf8))
                return PathRejection::InvalidUtf8;
            return std::nullopt;
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
        friend class RelativePath;
        friend class RemotePath;
        friend class LocalPath;

        /**
         * @brief Takes @p value as it is, for text a path type already validated.
         */
        explicit PathSegment(std::string value)
            : value_{std::move(value)}
        {}

        std::string value_;
    };
}
