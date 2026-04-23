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
     * @brief One node of a scan tree.
     *
     * Built on the backend by @ref Utility::TreeDirectoryWalker; never serialized to the
     * frontend. Children are sorted ascending by @ref name so a parallel merge walk can
     * diff against another sorted ScanNode without intermediate maps.
     */
    struct ScanNode
    {
        /// Final path segment. Empty for the root.
        std::string name{};
        FileType type{FileType::Unknown};
        std::uint64_t size{0};
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
        std::filesystem::perms permissions{std::filesystem::perms::unknown};
        /// For symlinks: the raw link literal. What the diff compares by.
        std::optional<std::filesystem::path> linkTarget{};
        std::uint32_t uid{0};
        std::uint32_t gid{0};
        /// Sorted ascending by name (POSIX byte comparison).
        std::vector<ScanNode> children{};
        /// Cached subtree metrics — populated once at build time, reused by the
        /// diff to emit "upload-only subtree" summaries without re-walking.
        std::uint64_t subtreeFileCount{0};
        std::uint64_t subtreeByteTotal{0};
        /// False when the scan did not descend into this directory (non-recursive mode).
        /// Diff suppresses one-sided delete emissions when this is false.
        bool childrenKnown{true};
    };
}
