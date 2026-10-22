#include <backend/file_tracking/disk_place.hpp>

#include <algorithm>
#include <memory>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <sys/stat.h>
#endif

namespace FileTracking
{
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
