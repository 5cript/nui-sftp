#include <utility/link_safe_remove.hpp>
#include <utility/no_follow_tree_walk.hpp>
#include "no_follow_inspection.hpp"

#include <chrono>
#include <memory>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif

namespace Utility
{
    namespace Detail
    {
        bool isLinkReparseTag(std::uint32_t tag) noexcept
        {
            // IsReparseTagNameSurrogate: the item stands for another named item, like a symlink or junction does.
            constexpr std::uint32_t nameSurrogateBit = 0x20000000u;
            return (tag & nameSurrogateBit) != 0;
        }

#ifdef _WIN32
        namespace
        {
            struct FindCloser
            {
                void operator()(HANDLE handle) const noexcept
                {
                    if (handle != INVALID_HANDLE_VALUE)
                        ::FindClose(handle);
                }
            };
            using FindHandle = std::unique_ptr<void, FindCloser>;

            bool isMissingError(DWORD lastError)
            {
                return lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND;
            }
        }

        NoFollowInspection inspectNoFollow(std::filesystem::path const& path, std::error_code& error)
        {
            error.clear();
            const auto attributes = ::GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto lastError = ::GetLastError();
                if (!isMissingError(lastError))
                    error = std::error_code{static_cast<int>(lastError), std::system_category()};
                return {};
            }
            const bool directoryEntry = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                WIN32_FIND_DATAW data{};
                const FindHandle find{::FindFirstFileW(path.c_str(), &data)};
                if (find.get() == INVALID_HANDLE_VALUE)
                {
                    const auto lastError = ::GetLastError();
                    if (!isMissingError(lastError))
                        error = std::error_code{static_cast<int>(lastError), std::system_category()};
                    return {};
                }
                if (isLinkReparseTag(data.dwReserved0))
                    return {.kind = NoFollowKind::Link, .directoryEntry = directoryEntry};
            }
            return {
                .kind = directoryEntry ? NoFollowKind::Directory : NoFollowKind::Other,
                .directoryEntry = directoryEntry,
            };
        }

        bool removeInspected(std::filesystem::path const& path, NoFollowInspection const& inspection, std::error_code& error)
        {
            error.clear();
            if (inspection.kind == NoFollowKind::Missing)
                return false;
            // A junction or directory link is removed like an empty directory, its target stays.
            const auto removeOnce = [&path, &inspection]()
            {
                return inspection.directoryEntry ? ::RemoveDirectoryW(path.c_str()) != 0 : ::DeleteFileW(path.c_str()) != 0;
            };
            if (removeOnce())
                return true;
            auto lastError = ::GetLastError();
            if (isMissingError(lastError))
                return false;
            if (lastError == ERROR_ACCESS_DENIED && inspection.kind != NoFollowKind::Link)
            {
                const auto attributes = ::GetFileAttributesW(path.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0)
                {
                    const auto writable = attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
                    if (::SetFileAttributesW(path.c_str(), writable == 0 ? FILE_ATTRIBUTE_NORMAL : writable) != 0)
                    {
                        if (removeOnce())
                            return true;
                        lastError = ::GetLastError();
                        ::SetFileAttributesW(path.c_str(), attributes);
                    }
                }
            }
            error = std::error_code{static_cast<int>(lastError), std::system_category()};
            return false;
        }
#else
        NoFollowInspection inspectionOf(std::filesystem::file_status const& status)
        {
            switch (status.type())
            {
                case std::filesystem::file_type::none:
                case std::filesystem::file_type::not_found:
                    return {};
                case std::filesystem::file_type::symlink:
                    return {.kind = NoFollowKind::Link};
                case std::filesystem::file_type::directory:
                    return {.kind = NoFollowKind::Directory, .directoryEntry = true};
                default:
                    return {.kind = NoFollowKind::Other};
            }
        }

        NoFollowInspection inspectNoFollow(std::filesystem::path const& path, std::error_code& error)
        {
            const auto status = std::filesystem::symlink_status(path, error);
            if (status.type() == std::filesystem::file_type::not_found)
                error.clear();
            if (error)
                return {};
            return inspectionOf(status);
        }

        bool removeInspected(std::filesystem::path const& path, NoFollowInspection const& inspection, std::error_code& error)
        {
            error.clear();
            if (inspection.kind == NoFollowKind::Missing)
                return false;
            // remove(3) unlinks a link itself and removes a directory only when it is empty.
            return std::filesystem::remove(path, error);
        }
#endif
    }

    NoFollowKind noFollowKindOf(std::filesystem::path const& path, std::error_code& error)
    {
        return Detail::inspectNoFollow(path, error).kind;
    }

    bool isLinkOrJunction(std::filesystem::path const& path, std::error_code& error)
    {
        return noFollowKindOf(path, error) == NoFollowKind::Link;
    }

    bool VerifiedDirectories::contains(std::filesystem::path const& directory) const
    {
        const std::scoped_lock lock{mutex_};
        return directories_.contains(directory);
    }

    void VerifiedDirectories::insert(std::filesystem::path const& directory)
    {
        const std::scoped_lock lock{mutex_};
        directories_.insert(directory);
    }

    std::optional<std::filesystem::path> firstLinkBelow(
        std::filesystem::path const& root,
        std::filesystem::path const& path,
        std::error_code& error,
        VerifiedDirectories* verified
    )
    {
        error.clear();
        const auto relative = path.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..")
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return std::nullopt;
        }
        auto current = root;
        for (auto const& component : relative)
        {
            if (component == ".")
                continue;
            current /= component;
            if (verified && verified->contains(current))
                continue;
            const auto kind = noFollowKindOf(current, error);
            if (error)
                return std::nullopt;
            switch (kind)
            {
                case NoFollowKind::Link:
                    return current;
                case NoFollowKind::Missing:
                    return std::nullopt;
                case NoFollowKind::Directory:
                {
                    if (verified)
                        verified->insert(current);
                    break;
                }
                case NoFollowKind::Other:
                    break;
            }
        }
        return std::nullopt;
    }

    bool removeNoFollow(std::filesystem::path const& path, std::error_code& error)
    {
        const auto inspection = Detail::inspectNoFollow(path, error);
        if (error)
            return false;
        return Detail::removeInspected(path, inspection, error);
    }

    RemoveAllResult removeAllNoFollow(std::filesystem::path const& path)
    {
        NoFollowTreeWalk walk{path, NoFollowTreeWalk::Mode::Remove};
        walk.step(std::chrono::steady_clock::time_point::max());
        return {.removed = walk.entries(), .failures = walk.failures()};
    }

    std::uintmax_t removeAllNoFollow(std::filesystem::path const& path, std::error_code& error)
    {
        auto result = removeAllNoFollow(path);
        error = result.failures.empty() ? std::error_code{} : result.failures.front().error;
        return result.removed;
    }
}
