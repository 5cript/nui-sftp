#pragma once

#include <shared_data/directory_entry.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace SharedData::Sync
{
    /**
     * @brief Whether the scan knows what is behind a node. Sync never acts on a node that is not Listed.
     */
    enum class Availability
    {
        Listed,
        /** @brief A directory whose listing failed, its content is unknown. */
        Unreadable,
        /**
         * @brief Present but not synchronized: sockets, devices, FIFOs, links and junctions on Windows, and transfer
         *        temp files.
         */
        Excluded,
        /** @brief A name that cannot be a path segment, like ".." or a name containing '/'. */
        InvalidName,
    };

    /**
     * @brief One node of a scan tree.
     *
     * Built on the backend by @ref TreeDirectoryWalker; never serialized to the
     * frontend. Children are sorted ascending by @ref name so a parallel merge walk can
     * diff against another sorted ScanNode without intermediate maps.
     */
    struct ScanNode
    {
        /** @brief Final path segment. Empty for the root. */
        std::string name{};
        FileType type{FileType::Unknown};
        std::uint64_t size{0};
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
        std::filesystem::perms permissions{std::filesystem::perms::unknown};
        /** @brief For symlinks: the raw link literal. What the diff compares by. */
        std::optional<std::string> linkTarget{};
        std::uint32_t uid{0};
        std::uint32_t gid{0};
        /** @brief Sorted ascending by name (POSIX byte comparison). */
        std::vector<ScanNode> children{};
        /**
         * @brief Subtree metrics, computed once when the subtree is complete. The diff sums one-sided subtrees from
         *        them.
         */
        std::uint64_t subtreeFileCount{0};
        std::uint64_t subtreeByteTotal{0};
        /**
         * @brief False when the scan did not descend into this directory (non-recursive mode). The diff then emits no
         *        one-sided deletes below it.
         */
        bool childrenKnown{true};
        Availability availability{Availability::Listed};
    };
}
