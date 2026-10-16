#pragma once

#include <paths/relative_path.hpp>

#include <fmt/format.h>

#include <algorithm>
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
     * Joining onto an unset path gives an unset path.
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

            std::string value;
            value.reserve(utf8.size());
            for (auto const name : Detail::splitAtSlashes(utf8))
            {
                if (name.empty() || name == ".")
                    continue;
                if (const auto rejection = PathSegment::rejectionOf(name); rejection)
                    return refuse(*rejection);
                value.push_back('/');
                value.append(name);
            }
            if (value.empty())
                return root();
            return RemotePath{std::move(value)};
        }

        RemotePath operator/(PathSegment const& segment) const
        {
            if (empty())
                return {};
            RemotePath result{*this};
            Detail::appendSegment(result.value_, segment.value());
            return result;
        }

        RemotePath operator/(RelativePath const& relative) const
        {
            if (empty() || relative.empty())
                return *this;
            RemotePath result{*this};
            Detail::appendSegment(result.value_, relative.value());
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
            return PathSegment::make(fmt::format("{}{}", name->view(), suffix))
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

            std::string value = linkContent.front() == '/' ? std::string{} : parent().value_or(root()).value_;
            if (value == "/")
                value.clear();
            for (auto const name : Detail::splitAtSlashes(linkContent))
            {
                if (name.empty() || name == ".")
                    continue;
                if (name == "..")
                {
                    value.erase(std::min(value.rfind('/'), value.size()));
                    continue;
                }
                if (const auto rejection = PathSegment::rejectionOf(name); rejection)
                    return refuse(*rejection);
                value.push_back('/');
                value.append(name);
            }
            if (value.empty())
                return root();
            return RemotePath{std::move(value)};
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
            return PathSegment{std::string{Detail::lastSegment(value_)}};
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
