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
            RelativePath result;
            if (utf8.empty())
                return result;
            if (utf8.front() == '/')
                return std::unexpected(PathError{.reason = PathRejection::EscapesRoot, .input = std::string{utf8}});

            std::size_t start = 0;
            while (start <= utf8.size())
            {
                const auto end = std::min(utf8.find('/', start), utf8.size());
                auto segment = PathSegment::make(utf8.substr(start, end - start));
                if (!segment)
                    return std::unexpected(PathError{.reason = segment.error().reason, .input = std::string{utf8}});
                result = result / *segment;
                start = end + 1;
            }
            return result;
        }

        RelativePath operator/(PathSegment const& segment) const
        {
            RelativePath result{*this};
            if (!result.value_.empty())
                result.value_.push_back('/');
            result.value_ += segment.value();
            return result;
        }

        RelativePath operator/(RelativePath const& other) const
        {
            if (other.empty())
                return *this;
            if (empty())
                return other;
            RelativePath result{*this};
            result.value_.push_back('/');
            result.value_ += other.value_;
            return result;
        }

        bool empty() const noexcept
        {
            return value_.empty();
        }

        std::vector<PathSegment> segments() const
        {
            std::vector<PathSegment> result;
            std::size_t start = 0;
            while (!value_.empty() && start <= value_.size())
            {
                const auto end = std::min(value_.find('/', start), value_.size());
                result.push_back(*PathSegment::make(std::string_view{value_}.substr(start, end - start)));
                start = end + 1;
            }
            return result;
        }

        std::optional<RelativePath> parent() const
        {
            if (empty())
                return std::nullopt;
            const auto slash = value_.rfind('/');
            RelativePath result;
            if (slash != std::string::npos)
                result.value_ = value_.substr(0, slash);
            return result;
        }

        std::optional<PathSegment> leaf() const
        {
            if (empty())
                return std::nullopt;
            const auto slash = value_.rfind('/');
            return *PathSegment::make(slash == std::string::npos ? value_ : value_.substr(slash + 1));
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
        std::string value_{};
    };
}
