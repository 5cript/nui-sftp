#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace SharedData::Sync
{
    /**
     * @brief Why the diff left an entry alone even though it exists on one or both sides.
     */
    BOOST_DEFINE_ENUM_CLASS(BlockReason, Unreadable, Excluded, InvalidName, TypeConflict, CaseConflict)

    /**
     * @brief An entry sync does not act on, with the reason.
     */
    struct BlockedEntry
    {
        std::string relKey{};
        BlockReason reason{BlockReason::Unreadable};
    };
    BOOST_DESCRIBE_STRUCT(BlockedEntry, (), (relKey, reason))

    /**
     * @brief Aggregate metrics for one section (uploads, downloads, or deletes) of a diff.
     */
    struct SectionSummary
    {
        /** @brief Total number of actionable entries in the whole section (leaves + directories). */
        std::uint64_t itemCount{0};
        /**
         * @brief Sum of transfer bytes across the section. Directories contribute their subtree total; files contribute
         *        their own size.
         */
        std::uint64_t transferBytes{0};
        /** @brief Number of top-level rows the frontend should seed into the tree. */
        std::uint64_t rootChildCount{0};
    };
    BOOST_DESCRIBE_STRUCT(SectionSummary, (), (itemCount, transferBytes, rootChildCount))

    /**
     * @brief Returned synchronously from recomputeSyncDiff: the totals of each section. The rows themselves are
     *        loaded per directory.
     */
    struct DiffSummary
    {
        Ids::SyncSessionId sessionId{};
        SectionSummary uploads{};
        SectionSummary downloads{};
        SectionSummary deletes{};
        /** @brief Count of scan-node pairs/singletons visited during the merge walk. */
        std::uint64_t entriesCompared{0};
        /**
         * @brief Monotonic token. Frontend passes this back on subsequent loadChildren calls so stale requests issued
         *        before a new recompute can be rejected.
         */
        std::uint64_t generation{0};
        /**
         * @brief Backend-side decision: input was large enough that Comparing-phase progress events were emitted.
         *        Frontend uses this for telemetry only; rendering the Comparing phase is unconditional.
         */
        bool heavyCompare{false};
        /** @brief True when the walk bailed out early because of a cancel request. */
        bool cancelled{false};
        /** @brief Number of entries sync skips because they are unreadable, excluded or conflicting. */
        std::uint64_t blockedCount{0};
        /** @brief The first skipped entries, for the dialog to name them. */
        std::vector<BlockedEntry> blockedSamples{};
    };
    BOOST_DESCRIBE_STRUCT(
        DiffSummary,
        (),
        (sessionId,
         uploads,
         downloads,
         deletes,
         entriesCompared,
         generation,
         heavyCompare,
         cancelled,
         blockedCount,
         blockedSamples)
    )

    // ADL hooks — nlohmann's adl_serializer looks for to_json/from_json in the type's
    // namespace, and SharedData::to_json<T> lives one namespace up.  These forwarders
    // let it find the generic described-struct path.
    inline void to_json(nlohmann::json& j, BlockReason const& e)
    {
        SharedData::to_json(j, e);
    }
    inline void from_json(nlohmann::json const& j, BlockReason& e)
    {
        SharedData::from_json(j, e);
    }
    inline void to_json(nlohmann::json& j, BlockedEntry const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, BlockedEntry& s)
    {
        SharedData::from_json(j, s);
    }
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
    inline void to_val(Nui::val& v, BlockReason const& e)
    {
        SharedData::to_val(v, e);
    }
    inline void from_val(Nui::val const& v, BlockReason& e)
    {
        SharedData::from_val(v, e);
    }
    // No to_val/from_val forwarders for SectionSummary / DiffSummary — the
    // described-struct overload in nui's convertToVal handles them natively.
#endif
}
