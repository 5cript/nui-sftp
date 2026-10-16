#pragma once

#include <paths/local_path_segment.hpp>
#include <paths/native.hpp>
#include <paths/relative_path.hpp>

#include <algorithm>
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
    namespace Detail
    {
        /**
         * @brief True when @p text starts with @p prefix, ASCII letters compared without case.
         */
        inline bool startsWithIgnoringCase(std::string_view text, std::string_view prefix)
        {
            const auto folded = [](char character)
            {
                return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
            };
            return text.size() >= prefix.size() &&
                std::ranges::equal(text.substr(0, prefix.size()), prefix, {}, folded, folded);
        }

        /**
         * @brief Removes a \\?\ prefix from @p generic, a '/' separated Windows path: "//?/C:/x" becomes "C:/x"
         *        and "//?/UNC/server/share" becomes "//server/share". DevicePath for the device namespace "//./",
         *        any other "//?/" path and a server named ".", "?" or "..".
         */
        inline std::expected<void, PathRejection> removeWin32Prefix(std::string& generic)
        {
            const auto isLetter = [](char character)
            {
                return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
            };
            if (generic.starts_with("//?/"))
            {
                const auto rest = std::string_view{generic}.substr(4);
                if (startsWithIgnoringCase(rest, "unc/"))
                    generic.erase(2, 6);
                else if (rest.size() >= 2 && isLetter(rest[0]) && rest[1] == ':')
                    generic.erase(0, 4);
                else
                    return std::unexpected(PathRejection::DevicePath);
            }
            if (!generic.starts_with("//"))
                return {};
            const auto server = std::string_view{generic}.substr(2, generic.find('/', 2) - 2);
            if (server == "." || server == "?" || server == "..")
                return std::unexpected(PathRejection::DevicePath);
            return {};
        }

        /**
         * @brief Length of the root prefix of @p generic ("/", "C:/", "C:" or "//server/share"), 0 when it is not
         *        absolute on @p platform. Expects '/' separators and no \\?\ prefix.
         */
        inline std::size_t rootLengthOf(std::string_view generic, LocalPlatform platform)
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

        /**
         * @brief True when @p path is @p base or lies below it, both generic path text with @p base ending in '/'
         *        only when it is a root. @p equal compares two characters.
         */
        template <typename EqualT>
        bool isWithinText(std::string_view path, std::string_view base, EqualT equal)
        {
            if (path.size() < base.size() || !std::ranges::equal(path.substr(0, base.size()), base, equal))
                return false;
            return path.size() == base.size() || base.back() == '/' || path[base.size()] == '/';
        }
    }

    /**
     * @brief An absolute, normalized path on the local machine, kept as generic UTF-8 ('/' separated).
     *
     * The frontend runs in WebAssembly where std::filesystem would misread "C:/x", so the type does its own parsing
     * for both platforms. The backend converts to a native std::filesystem::path only through @ref native.
     *
     * On Windows the drive letter is upper case, a \\?\ prefix is dropped (\\?\UNC\server\share becomes
     * //server/share) and device paths like \\.\PhysicalDrive0 are refused.
     *
     * A default constructed LocalPath is unset (@ref empty), every operation that touches the disk must refuse it.
     * Joining onto an unset path gives an unset path.
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
            if (utf8.find('\0') != std::string_view::npos)
                return refuse(PathRejection::ContainsNul);
            if (!isValidUtf8(utf8))
                return refuse(PathRejection::InvalidUtf8);

            std::string generic{utf8};
            if (platform == LocalPlatform::Windows)
            {
                std::ranges::replace(generic, '\\', '/');
                if (const auto removed = Detail::removeWin32Prefix(generic); !removed)
                    return refuse(removed.error());
            }

            const auto rootLength = Detail::rootLengthOf(generic, platform);
            if (rootLength == 0)
                return refuse(PathRejection::NotAbsolute);

            std::vector<std::string_view> names;
            for (auto const name : Detail::splitAtSlashes(std::string_view{generic}.substr(rootLength)))
            {
                if (name.empty() || name == ".")
                    continue;
                if (name == "..")
                {
                    if (names.empty())
                        return refuse(PathRejection::EscapesRoot);
                    names.pop_back();
                    continue;
                }
                if (const auto rejection = LocalPathSegment::rejectionOf(name, platform); rejection)
                    return refuse(*rejection);
                names.push_back(name);
            }

            LocalPath result;
            result.platform_ = platform;
            result.value_.reserve(generic.size() + 1);
            result.value_.append(generic, 0, rootLength);
            if (result.value_.back() != '/')
                result.value_.push_back('/');
            if (platform == LocalPlatform::Windows && result.value_[0] >= 'a' && result.value_[0] <= 'z')
                result.value_[0] = static_cast<char>(result.value_[0] - 'a' + 'A');
            result.rootLength_ = result.value_.size();
            for (auto const name : names)
                Detail::appendSegment(result.value_, name);
            return result;
        }

#ifndef __EMSCRIPTEN__
        /**
         * @brief Takes a native path, which has to be absolute. Refused with InvalidUtf8 when its text is not
         *        UTF-8 (POSIX) or has an unpaired surrogate (Windows). Never throws.
         */
        static std::expected<LocalPath, PathError> make(std::filesystem::path const& path)
        {
            return checkedNativeToUtf8(path).and_then(
                [](std::string const& utf8)
                {
                    return fromUtf8(utf8);
                }
            );
        }

        std::filesystem::path native() const
        {
            return nativeFromUtf8(value_).make_preferred();
        }
#endif

        LocalPath operator/(LocalPathSegment const& segment) const
        {
            if (empty())
                return {};
            LocalPath result{*this};
            Detail::appendSegment(result.value_, segment.value());
            return result;
        }

        /**
         * @brief Joins @p relative, every segment has to be a valid name on this path's platform. Empty when this
         *        path is unset.
         */
        std::expected<LocalPath, PathError> join(RelativePath const& relative) const
        {
            if (empty())
                return std::unexpected(PathError{.reason = PathRejection::Empty, .input = relative.value()});
            for (auto const name : relative.segmentViews())
            {
                if (const auto rejection = LocalPathSegment::rejectionOf(name, platform_); rejection)
                    return std::unexpected(PathError{.reason = *rejection, .input = std::string{name}});
            }
            LocalPath result{*this};
            if (!relative.empty())
                Detail::appendSegment(result.value_, relative.value());
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
            return PathSegment{std::string{Detail::lastSegment(value_)}};
        }

        /**
         * @brief True when this is @p base or lies below it, compared exactly.
         */
        bool isWithin(LocalPath const& base) const
        {
            if (empty() || base.empty() || platform_ != base.platform_)
                return false;
            return Detail::isWithinText(value_, base.value_, std::ranges::equal_to{});
        }

        /**
         * @brief @ref isWithin with ASCII letters compared without case. For checks that must not miss a path the
         *        filesystem treats as the same, like the delete guards on Windows and macOS, which fold case.
         */
        bool isWithinIgnoringCase(LocalPath const& base) const
        {
            if (empty() || base.empty() || platform_ != base.platform_)
                return false;
            const auto folded = [](char character)
            {
                return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
            };
            return Detail::isWithinText(
                value_,
                base.value_,
                [&folded](char left, char right)
                {
                    return folded(left) == folded(right);
                }
            );
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
        std::string value_{};
        std::size_t rootLength_{0};
        LocalPlatform platform_{LocalPlatform::Posix};
    };
}
