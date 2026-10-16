#pragma once

#include <paths/relative_path.hpp>

#include <cassert>
#include <compare>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Paths
{
    /**
     * @brief An absolute path on the server: UTF-8, '/' separated, normalized.
     *
     * Never touches std::filesystem, so a backslash or a colon in a server name stays a character on every client
     * platform. ".." is refused instead of resolved: the server decides what it means.
     *
     * A default constructed RemotePath is unset (@ref empty), every operation that touches the server must refuse it.
     */
    class RemotePath
    {
      public:
        RemotePath() = default;

        static RemotePath root()
        {
            return RemotePath{std::string{"/"}};
        }

        /**
         * @brief Collapses repeated separators and "." segments and drops a trailing '/'.
         */
        static std::expected<RemotePath, PathError> make(std::string_view utf8)
        {
            const auto refuse = [utf8](PathRejection reason)
            {
                return std::unexpected(PathError{.reason = reason, .input = std::string{utf8}});
            };
            if (utf8.empty())
                return refuse(PathRejection::Empty);
            if (utf8.front() != '/')
                return refuse(PathRejection::NotAbsolute);

            auto result = root();
            std::size_t start = 1;
            while (start <= utf8.size())
            {
                const auto end = std::min(utf8.find('/', start), utf8.size());
                const auto name = utf8.substr(start, end - start);
                start = end + 1;
                if (name.empty() || name == ".")
                    continue;
                auto segment = PathSegment::make(name);
                if (!segment)
                    return refuse(segment.error().reason);
                result = result / *segment;
            }
            return result;
        }

        RemotePath operator/(PathSegment const& segment) const
        {
            assert(!empty());
            RemotePath result{*this};
            if (result.value_.back() != '/')
                result.value_.push_back('/');
            result.value_ += segment.value();
            return result;
        }

        RemotePath operator/(RelativePath const& relative) const
        {
            if (relative.empty())
                return *this;
            assert(!empty());
            RemotePath result{*this};
            if (result.value_.back() != '/')
                result.value_.push_back('/');
            result.value_ += relative.value();
            return result;
        }

        /**
         * @brief This path with @p suffix appended to its last name, like "file.txt" to "file.txt.filepart".
         */
        std::expected<RemotePath, PathError> withFilenameSuffix(std::string_view suffix) const
        {
            const auto name = filename();
            if (!name)
                return std::unexpected(PathError{.reason = PathRejection::Empty, .input = value_});
            return PathSegment::make(name->value() + std::string{suffix})
                .transform(
                    [this](PathSegment const& segment)
                    {
                        return *parent() / segment;
                    }
                );
        }

        /**
         * @brief Where a symlink at this path with the content @p linkContent points to.
         *
         * Relative content is taken from the link's directory. ".." is resolved lexically, which differs from the
         * server's view only when a directory on the way is itself a symlink. ".." at the root stays at the root.
         */
        std::expected<RemotePath, PathError> resolveLink(std::string_view linkContent) const
        {
            const auto refuse = [linkContent](PathRejection reason)
            {
                return std::unexpected(PathError{.reason = reason, .input = std::string{linkContent}});
            };
            if (linkContent.empty())
                return refuse(PathRejection::Empty);
            if (empty())
                return refuse(PathRejection::NotAbsolute);

            auto result = linkContent.front() == '/' ? root() : parent().value_or(root());
            std::size_t start = 0;
            while (start <= linkContent.size())
            {
                const auto end = std::min(linkContent.find('/', start), linkContent.size());
                const auto name = linkContent.substr(start, end - start);
                start = end + 1;
                if (name.empty() || name == ".")
                    continue;
                if (name == "..")
                {
                    result = result.parent().value_or(root());
                    continue;
                }
                auto segment = PathSegment::make(name);
                if (!segment)
                    return refuse(segment.error().reason);
                result = result / *segment;
            }
            return result;
        }

        bool empty() const noexcept
        {
            return value_.empty();
        }

        bool isRoot() const noexcept
        {
            return value_ == "/";
        }

        std::optional<RemotePath> parent() const
        {
            if (empty() || isRoot())
                return std::nullopt;
            const auto slash = value_.rfind('/');
            return RemotePath{slash == 0 ? std::string{"/"} : value_.substr(0, slash)};
        }

        std::optional<PathSegment> filename() const
        {
            if (empty() || isRoot())
                return std::nullopt;
            return *PathSegment::make(std::string_view{value_}.substr(value_.rfind('/') + 1));
        }

        /**
         * @brief True when this is @p base or lies below it.
         */
        bool isWithin(RemotePath const& base) const
        {
            if (empty() || base.empty())
                return false;
            if (base.isRoot() || value_ == base.value_)
                return true;
            return value_.size() > base.value_.size() && value_.starts_with(base.value_) &&
                value_[base.value_.size()] == '/';
        }

        /**
         * @brief The path below @p base, EscapesRoot when this is not within it.
         */
        std::expected<RelativePath, PathError> relativeTo(RemotePath const& base) const
        {
            if (!isWithin(base))
                return std::unexpected(PathError{.reason = PathRejection::EscapesRoot, .input = value_});
            if (value_ == base.value_)
                return RelativePath{};
            const auto offset = base.isRoot() ? 1u : base.value_.size() + 1u;
            return RelativePath::make(std::string_view{value_}.substr(offset));
        }

        std::string const& value() const noexcept
        {
            return value_;
        }

        std::string_view view() const noexcept
        {
            return value_;
        }

        char const* c_str() const noexcept
        {
            return value_.c_str();
        }

        auto operator<=>(RemotePath const&) const = default;

      private:
        explicit RemotePath(std::string value)
            : value_{std::move(value)}
        {}

        std::string value_{};
    };
}
