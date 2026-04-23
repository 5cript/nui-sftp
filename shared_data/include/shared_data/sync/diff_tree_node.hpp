#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/sync/diff.hpp>

#include <utility/describe.hpp>

#include <cstdint>
#include <string>

namespace SharedData::Sync
{
    BOOST_DEFINE_ENUM_CLASS(DiffSection, Upload, Download, Delete)

    /**
     * @brief One row of the diff tree, as sent to the frontend.
     *
     * Carries just enough to render the row and drive selection — not the full
     * DirectoryEntry. One-sided directories emit a single node whose
     * @ref directChildCount / @ref descendantItemCount describe the subtree
     * without forcing the walk to recurse.
     *
     * The frontend treats @ref relKey as the stable node id (doubles as the
     * selection key).
     */
    struct DiffTreeNode
    {
        /// Stable identifier. Posix-relative path from the scan root.
        std::string relKey{};
        /// Final path segment — what the row renders.
        std::string name{};
        Action action{Action::Upload};
        bool isDirectory{false};
        bool hasLocalSide{false};
        bool hasRemoteSide{false};
        std::uint64_t localSize{0};
        std::uint64_t remoteSize{0};
        std::uint64_t localMtime{0};
        std::uint64_t remoteMtime{0};
        /// 0 = leaf or fully-resolved empty dir; >0 = lazy-loadable via
        /// loadSyncDiffChildren.
        std::uint32_t directChildCount{0};
        /// For one-sided directory emissions: number of actionable descendants
        /// (files). Lets the row show a subtree summary without round-tripping.
        std::uint64_t descendantItemCount{0};
        std::uint64_t descendantByteTotal{0};
        /// Synthesized ancestor row whose sole purpose is to let the frontend
        /// build the tree hierarchy above differing leaves (a/b/c.txt → needs
        /// rows for 'a' and 'a/b').  Has no action; its 'action' field is
        /// meaningless. Both hasLocalSide and hasRemoteSide are true.
        bool isStructural{false};
    };
    BOOST_DESCRIBE_STRUCT(
        DiffTreeNode,
        (),
        (relKey,
         name,
         action,
         isDirectory,
         hasLocalSide,
         hasRemoteSide,
         localSize,
         remoteSize,
         localMtime,
         remoteMtime,
         directChildCount,
         descendantItemCount,
         descendantByteTotal,
         isStructural)
    )

    inline void to_json(nlohmann::json& j, DiffTreeNode const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, DiffTreeNode& s)
    {
        SharedData::from_json(j, s);
    }

    // Enum-as-string ADL bridges — nlohmann's adl_serializer for DiffSection
    // looks for free functions in its own namespace, and SharedData::to_json<EnumT>
    // (generic) isn't found by ADL from nested namespaces.
    inline void to_json(nlohmann::json& j, DiffSection const& e)
    {
        SharedData::to_json(j, e);
    }
    inline void from_json(nlohmann::json const& j, DiffSection& e)
    {
        SharedData::from_json(j, e);
    }

#ifdef NUI_FRONTEND
    inline void to_val(Nui::val& v, DiffSection const& e)
    {
        SharedData::to_val(v, e);
    }
    inline void from_val(Nui::val const& v, DiffSection& e)
    {
        SharedData::from_val(v, e);
    }
    // No to_val/from_val forwarder for DiffTreeNode — described-struct path
    // in nui's convertToVal handles it natively.
#endif
}
