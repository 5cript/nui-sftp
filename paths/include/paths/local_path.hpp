#pragma once

#include <paths/local_path_segment.hpp>
#include <paths/relative_path.hpp>

#include <cassert>
#include <compare>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef __EMSCRIPTEN__
#    include <filesystem>
#endif

namespace Paths
{
    /**
     * @brief An absolute, normalized path on the local machine, kept as generic UTF-8 ('/' separated).
     *
     * The frontend runs in WebAssembly where std::filesystem would misread "C:/x", so the type does its own parsing
     * for both platforms. The backend converts to a native std::filesystem::path only through @ref native.
     *
     * A default constructed LocalPath is unset (@ref empty), every operation that touches the disk must refuse it.
     */
    class LocalPath
    {
      public:
        LocalPath() = default;

        static std::expected<LocalPath, PathError>
        fromUtf8(std::string_view utf8, LocalPlatform platform = defaultLocalPlatform())
        {
            const auto refuse = [utf8](PathRejection reason)
            {
                return std::unexpected(PathError{.reason = reason, .input = std::string{utf8}});
            };
            if (utf8.empty())
                return refuse(PathRejection::Empty);
            if (!isValidUtf8(utf8))
                return refuse(PathRejection::InvalidUtf8);

            std::string generic{utf8};
            if (platform == LocalPlatform::Windows)
                std::ranges::replace(generic, '\\', '/');

            auto rootLength = rootLengthOf(generic, platform);
            if (rootLength == 0)
                return refuse(PathRejection::NotAbsolute);

            LocalPath result;
            result.platform_ = platform;
            result.value_ = generic.substr(0, rootLength);
            if (result.value_.back() != '/')
                result.value_.push_back('/');
            result.rootLength_ = result.value_.size();

            std::vector<std::string_view> segments;
            std::string_view rest{generic};
            rest.remove_prefix(rootLength);
            std::size_t start = 0;
            while (start <= rest.size())
            {
                const auto end = std::min(rest.find('/', start), rest.size());
                const auto name = rest.substr(start, end - start);
                start = end + 1;
                if (name.empty() || name == ".")
                    continue;
                if (name == "..")
                {
                    if (segments.empty())
                        return refuse(PathRejection::EscapesRoot);
                    segments.pop_back();
                    continue;
                }
                segments.push_back(name);
            }
            for (auto const name : segments)
            {
                auto segment = LocalPathSegment::make(name, platform);
                if (!segment)
                    return refuse(segment.error().reason);
                result = result / *segment;
            }
            return result;
        }

#ifndef __EMSCRIPTEN__
        /**
         * @brief Takes a native path, which has to be absolute.
         */
        static std::expected<LocalPath, PathError> make(std::filesystem::path const& path)
        {
            const auto utf8 = path.generic_u8string();
            return fromUtf8(std::string_view{reinterpret_cast<char const*>(utf8.data()), utf8.size()});
        }

        std::filesystem::path native() const
        {
            return std::filesystem::path{
                std::u8string{reinterpret_cast<char8_t const*>(value_.data()), value_.size()}
            }.make_preferred();
        }
#endif

        LocalPath operator/(LocalPathSegment const& segment) const
        {
            assert(!empty());
            LocalPath result{*this};
            if (result.value_.back() != '/')
                result.value_.push_back('/');
            result.value_ += segment.value();
            return result;
        }

        /**
         * @brief Joins @p relative, every segment has to be a valid name on this path's platform.
         */
        std::expected<LocalPath, PathError> join(RelativePath const& relative) const
        {
            LocalPath result{*this};
            for (auto const& segment : relative.segments())
            {
                auto local = LocalPathSegment::make(segment, platform_);
                if (!local)
                    return std::unexpected(local.error());
                result = result / *local;
            }
            return result;
        }

        bool empty() const noexcept
        {
            return value_.empty();
        }

        bool isRoot() const noexcept
        {
            return !empty() && value_.size() == rootLength_;
        }

        std::optional<LocalPath> parent() const
        {
            if (empty() || isRoot())
                return std::nullopt;
            LocalPath result{*this};
            const auto slash = value_.rfind('/');
            result.value_ = slash + 1 <= rootLength_ ? value_.substr(0, rootLength_) : value_.substr(0, slash);
            return result;
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
        bool isWithin(LocalPath const& base) const
        {
            if (empty() || base.empty() || platform_ != base.platform_)
                return false;
            if (value_ == base.value_)
                return true;
            if (!value_.starts_with(base.value_))
                return false;
            return base.value_.back() == '/' || value_[base.value_.size()] == '/';
        }

        /**
         * @brief The path below @p base, EscapesRoot when this is not within it.
         */
        std::expected<RelativePath, PathError> relativeTo(LocalPath const& base) const
        {
            if (!isWithin(base))
                return std::unexpected(PathError{.reason = PathRejection::EscapesRoot, .input = value_});
            if (value_ == base.value_)
                return RelativePath{};
            auto rest = std::string_view{value_}.substr(base.value_.size());
            if (!rest.empty() && rest.front() == '/')
                rest.remove_prefix(1);
            return RelativePath::make(rest);
        }

        /**
         * @brief Generic UTF-8 form, '/' separated on every platform.
         */
        std::string const& toUtf8() const noexcept
        {
            return value_;
        }

        LocalPlatform platform() const noexcept
        {
            return platform_;
        }

        auto operator<=>(LocalPath const&) const = default;

      private:
        /**
         * @brief Length of the root prefix ("/", "C:/" or "//server/share"), 0 when not absolute.
         */
        static std::size_t rootLengthOf(std::string_view generic, LocalPlatform platform)
        {
            if (platform == LocalPlatform::Posix)
                return generic.starts_with('/') ? 1u : 0u;

            const auto isLetter = [](char character)
            {
                return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
            };
            if (generic.size() >= 3 && isLetter(generic[0]) && generic[1] == ':' && generic[2] == '/')
                return 3u;
            if (generic.size() == 2 && isLetter(generic[0]) && generic[1] == ':')
                return 2u;
            if (generic.starts_with("//"))
            {
                // UNC: //server/share is the root.
                const auto serverEnd = generic.find('/', 2);
                if (serverEnd == std::string_view::npos || serverEnd == 2)
                    return 0u;
                const auto shareEnd = std::min(generic.find('/', serverEnd + 1), generic.size());
                if (shareEnd == serverEnd + 1)
                    return 0u;
                return shareEnd;
            }
            return 0u;
        }

        std::string value_{};
        std::size_t rootLength_{0};
        LocalPlatform platform_{LocalPlatform::Posix};
    };
}
