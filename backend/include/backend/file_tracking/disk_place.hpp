#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace FileTracking
{
    /**
     * @brief Tells a file apart from every other one on the machine, whatever path leads to it.
     */
    struct DiskIdentity
    {
        std::uint64_t device;
        std::uint64_t file;

        friend auto operator<=>(DiskIdentity const&, DiskIdentity const&) = default;
    };

    /**
     * @brief The file at @p path, nothing when it does not exist or cannot be looked at.
     */
    std::optional<DiskIdentity> diskIdentityOf(std::filesystem::path const& path);

    /**
     * @brief Whether the file @p identity names was deleted rather than moved elsewhere on its volume, to the trash
     *        for one. Nothing when that cannot be told: only macOS looks a file up by its identity, on APFS and HFS+.
     * @param directory A directory on the same volume that is there, the one the file was in.
     */
    std::optional<bool> isDeletedFromItsVolume(DiskIdentity const& identity, std::filesystem::path const& directory);

    /**
     * @brief Where a path lies on disk: the identities of the path and of the directories above it that exist. Taken
     *        once, so comparing places costs no further look at the disk.
     */
    struct DiskPlace
    {
        /** @brief The path itself, nothing when it does not exist. */
        std::optional<DiskIdentity> self{};
        /** @brief The path and the directories above it that exist, the path first. */
        std::vector<DiskIdentity> chain{};

        static DiskPlace of(std::filesystem::path const& path);

        /**
         * @brief Whether this place is @p base or lies below it on disk. Nothing that does not exist is.
         */
        bool isAtOrBelow(DiskPlace const& base) const;
    };
}
