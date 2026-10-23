#include <backend/file_tracking/disk_place.hpp>
#include <utility/checked_symlink_status.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <memory>
#include <string_view>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <sys/stat.h>
#endif
#ifdef __APPLE__
#    include <sys/mount.h>
#endif

namespace FileTracking
{
#ifdef __APPLE__
    namespace
    {
        /**
         * @brief Whether @p directory lies on the volume @p device and that volume looks files up by their identity
         *        under /.vol.
         */
        bool looksUpByIdentity(std::filesystem::path const& directory, std::uint64_t device)
        {
            struct stat status{};
            struct statfs volume{};
            if (::stat(directory.c_str(), &status) != 0 || static_cast<std::uint64_t>(status.st_dev) != device ||
                ::statfs(directory.c_str(), &volume) != 0)
                return false;
            const std::string_view type{volume.f_fstypename};
            return type == "apfs" || type == "hfs";
        }
    }
#endif

    std::optional<DiskIdentity> diskIdentityOf(std::filesystem::path const& path)
    {
#ifdef _WIN32
        // Backup semantics open directories too.
        HANDLE const opened = CreateFileW(
            path.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr
        );
        if (opened == INVALID_HANDLE_VALUE)
            return std::nullopt;
        const auto handle = std::unique_ptr<void, decltype(&CloseHandle)>{opened, &CloseHandle};
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.get(), &information))
            return std::nullopt;
        return DiskIdentity{
            .device = information.dwVolumeSerialNumber,
            .file = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32u) | information.nFileIndexLow,
        };
#else
        struct stat status{};
        if (::stat(path.c_str(), &status) != 0)
            return std::nullopt;
        return DiskIdentity{
            .device = static_cast<std::uint64_t>(status.st_dev),
            .file = static_cast<std::uint64_t>(status.st_ino),
        };
#endif
    }

    std::optional<bool> isDeletedFromItsVolume(
        [[maybe_unused]] DiskIdentity const& identity,
        [[maybe_unused]] std::filesystem::path const& directory
    )
    {
#ifdef __APPLE__
        if (!looksUpByIdentity(directory, identity.device))
            return std::nullopt;
        // fsgetpath() still finds a deleted directory for a while after the last descriptor of it closed.
        const auto status = Utility::checkedSymlinkStatus(fmt::format("/.vol/{}/{}", identity.device, identity.file));
        if (!status)
            return std::nullopt;
        return !status->has_value();
#else
        return std::nullopt;
#endif
    }

    DiskPlace DiskPlace::of(std::filesystem::path const& path)
    {
        DiskPlace place{.self = diskIdentityOf(path), .chain = {}};
        if (place.self)
            place.chain.push_back(*place.self);
        for (auto ancestor = path; ancestor.has_relative_path();)
        {
            ancestor = ancestor.parent_path();
            if (const auto identity = diskIdentityOf(ancestor))
                place.chain.push_back(*identity);
        }
        return place;
    }

    bool DiskPlace::isAtOrBelow(DiskPlace const& base) const
    {
        return base.self && std::ranges::contains(chain, *base.self);
    }
}
