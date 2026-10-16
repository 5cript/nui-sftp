#pragma once

#include <shared_data/sync/diff.hpp>

#include <cstdint>
#include <string>

namespace SharedData::Sync
{
    /**
     * @brief One entry of the plan @ref DiffTreeStore::buildPlan makes from a sparse selection.
     *
     * A transfer lists the files and symlinks of a selected directory, plus every directory that has no
     * transferred content and so has to be created by itself; the files create their parents. A delete lists a
     * directory before its listed content. Entries name rows by relKey, the backend resolves them below its own
     * sync roots.
     */
    struct EnqueuePlanEntry
    {
        std::string relKey{};
        Action action{Action::Upload};
        std::uint64_t sizeBytes{0};
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
        bool isDirectory{false};
    };
}
