#pragma once

#include <paths/path_segment.hpp>

#include <compare>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Paths
{
    /**
     * @brief A path below some root: zero or more segments joined by '/'. Empty is the root itself.
     *
     * It never starts with '/' and never contains "." or "..", so joining it onto a root cannot leave that root.
     * This is the only bridge between a local and a remote path.
     */
    class RelativePath
    {
      public:
        RelativePath() = default;

        static std::expected<RelativePath, PathError> make(std::string_view utf8)
        {
            if (utf8.empty())
                return RelativePath{};
            if (utf8.front() == '/')
                return std::unexpected(PathError{.reason = PathRejection::EscapesRoot, .input = std::string{utf8}});
            for (auto const part : Detail::splitAtSlashes(utf8))
            {
                if (const auto rejection = PathSegment::rejectionOf(part); rejection)
                    return std::unexpected(PathError{.reason = *rejection, .input = std::string{utf8}});
            }
            return RelativePath{std::string{utf8}};
        }

        RelativePath operator/(PathSegment const& segment) const
        {
            RelativePath result{*this};
            Detail::appendSegment(result.value_, segment.value());
            return result;
        }

        RelativePath operator/(RelativePath const& other) const
        {
            RelativePath result{*this};
            if (!other.empty())
                Detail::appendSegment(result.value_, other.value_);
            return result;
        }

        bool empty() const noexcept
        {
            return value_.empty();
        }

        /**
         * @brief The names as views into this path, without copying or validating them again.
         */
        auto segmentViews() const
        {
            return Detail::splitAtSlashes(value_);
        }

        std::vector<PathSegment> segments() const
        {
            std::vector<PathSegment> result;
            for (auto const part : segmentViews())
                result.push_back(PathSegment{std::string{part}});
            return result;
        }

        std::optional<RelativePath> parent() const
        {
            if (empty())
                return std::nullopt;
            const auto slash = value_.rfind('/');
            return RelativePath{slash == std::string::npos ? std::string{} : value_.substr(0, slash)};
        }

        std::optional<PathSegment> leaf() const
        {
            if (empty())
                return std::nullopt;
            return PathSegment{std::string{Detail::lastSegment(value_)}};
        }

        /**
         * @brief True when this is @p other or lies below it.
         */
        bool isWithin(RelativePath const& other) const
        {
            if (other.empty() || value_ == other.value_)
                return true;
            return value_.size() > other.value_.size() && value_.starts_with(other.value_) &&
                value_[other.value_.size()] == '/';
        }

        std::string const& value() const noexcept
        {
            return value_;
        }

        std::string_view view() const noexcept
        {
            return value_;
        }

        auto operator<=>(RelativePath const&) const = default;

      private:
        explicit RelativePath(std::string value)
            : value_{std::move(value)}
        {}

        std::string value_{};
    };
}
