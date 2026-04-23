#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <cstdint>

namespace SharedData::Sync
{
    /**
     * @brief Aggregate metrics for one section (uploads, downloads, or deletes) of a diff.
     */
    struct SectionSummary
    {
        /// Total number of actionable entries in the whole section (leaves + directories).
        std::uint64_t itemCount{0};
        /// Sum of transfer bytes across the section. Directories contribute their
        /// subtree total; files contribute their own size.
        std::uint64_t transferBytes{0};
        /// Number of top-level rows the frontend should seed into the tree.
        std::uint64_t rootChildCount{0};
    };
    BOOST_DESCRIBE_STRUCT(SectionSummary, (), (itemCount, transferBytes, rootChildCount))

    /**
     * @brief Returned synchronously from recomputeSyncDiff; replaces the old
     *        entries-per-section vectors that used to cross RPC.
     */
    struct DiffSummary
    {
        Ids::SyncSessionId sessionId{};
        SectionSummary uploads{};
        SectionSummary downloads{};
        SectionSummary deletes{};
        /// Count of scan-node pairs/singletons visited during the merge walk.
        std::uint64_t entriesCompared{0};
        /// Monotonic token. Frontend passes this back on subsequent loadChildren calls
        /// so stale requests issued before a new recompute can be rejected.
        std::uint64_t generation{0};
        /// Backend-side decision: input was large enough that Comparing-phase progress
        /// events were emitted. Frontend uses this for telemetry only; rendering the
        /// Comparing phase is unconditional.
        bool heavyCompare{false};
        /// True when the walk bailed out early because of a cancel request.
        bool cancelled{false};
    };
    BOOST_DESCRIBE_STRUCT(
        DiffSummary,
        (),
        (sessionId, uploads, downloads, deletes, entriesCompared, generation, heavyCompare, cancelled)
    )

    // ADL hooks — nlohmann's adl_serializer looks for to_json/from_json in the type's
    // namespace, and SharedData::to_json<T> lives one namespace up.  These forwarders
    // let it find the generic described-struct path.
    inline void to_json(nlohmann::json& j, SectionSummary const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, SectionSummary& s)
    {
        SharedData::from_json(j, s);
    }
    inline void to_json(nlohmann::json& j, DiffSummary const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, DiffSummary& s)
    {
        SharedData::from_json(j, s);
    }

#ifdef NUI_FRONTEND
    // No to_val/from_val forwarders for SectionSummary / DiffSummary — the
    // described-struct overload in nui's convertToVal handles them natively.
#endif
}
