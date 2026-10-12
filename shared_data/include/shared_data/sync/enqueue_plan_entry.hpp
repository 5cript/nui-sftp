#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/sync/diff.hpp>

#include <utility/describe.hpp>

#include <cstdint>
#include <string>

namespace SharedData::Sync
{
    /**
     * @brief One row of the backend-computed enqueue plan.
     *
     * The backend collapses the frontend's selected relKey set into a minimal list
     * that:
     *
     *  - emits a one-sided directory as a single bulk-dir entry when every descendant
     *    is selected (saves N transfers for N descendants),
     *  - emits an individual leaf otherwise,
     *  - skips intermediate directory rows with partial selection — the enqueue
     *    side creates missing directories as needed via the bulk upload/download
     *    flow.
     *
     * Entries carry no paths: the backend resolves @ref relKey below its own sync roots.
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
    BOOST_DESCRIBE_STRUCT(
        EnqueuePlanEntry,
        (),
        (relKey, action, sizeBytes, mtime, mtimeNsec, isDirectory)
    )

    inline void to_json(nlohmann::json& j, EnqueuePlanEntry const& e)
    {
        SharedData::to_json(j, e);
    }
    inline void from_json(nlohmann::json const& j, EnqueuePlanEntry& e)
    {
        SharedData::from_json(j, e);
    }

#ifdef NUI_FRONTEND
    // No to_val/from_val forwarder for EnqueuePlanEntry — described-struct path
    // in nui's convertToVal handles it natively.
#endif
}
