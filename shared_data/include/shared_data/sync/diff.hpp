#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/sync/scan_node.hpp>

#include <utility/describe.hpp>
#include <utility/enum_string_convert.hpp>

#include <cstdint>
#include <string>

namespace SharedData::Sync
{
    struct DiffTreeNode;

    BOOST_DEFINE_ENUM_CLASS(Direction, Both, Upload, Download)

    BOOST_DEFINE_ENUM_CLASS(Action, Upload, Download, DeleteLocal, DeleteRemote)

    /**
     * @brief Inputs that govern which differences become actionable items.
     */
    struct DiffOptions
    {
        Direction direction{Direction::Both};
        bool actionUpload{true};
        bool actionDownload{true};
        bool actionDelete{false};
        /// When false, entries below the root directory are dropped from the diff.
        bool recursive{true};
        /// When true, entries with any path segment starting with '.' are dropped.
        bool ignoreHidden{false};
    };
    BOOST_DESCRIBE_STRUCT(
        DiffOptions,
        (),
        (direction, actionUpload, actionDownload, actionDelete, recursive, ignoreHidden)
    )

    /**
     * @brief Sink that receives diff-tree emissions during @ref diffScanTrees.
     *
     * The implementation owns the destination storage and pagination layout.
     * Progress heartbeats are fired every @p checkpointInterval compares;
     * implementations can also ask the walk to exit early via @ref cancelled().
     */
    class DiffSink
    {
      public:
        virtual ~DiffSink() = default;

        virtual void emitUpload(DiffTreeNode node, std::string const& parentRelKey) = 0;
        virtual void emitDownload(DiffTreeNode node, std::string const& parentRelKey) = 0;
        virtual void emitDelete(DiffTreeNode node, std::string const& parentRelKey) = 0;

        /**
         * @brief Called periodically with the running compare count. Implementations
         *        typically translate this into a remote-progress RPC.
         */
        virtual void onProgress(std::uint64_t entriesCompared) = 0;

        /**
         * @brief Lets the walk bail out early. Default never-cancels implementation
         *        is provided so tests don't need to implement it.
         */
        virtual bool cancelled() const
        {
            return false;
        }
    };

    /**
     * @brief True when two scan nodes should count as a difference.
     *
     * Type mismatches always count. Symlinks compare by raw @ref ScanNode::linkTarget
     * (size/mtime of the link metadata itself is meaningless). Files compare by
     * (size, mtime). Directories alone never "differ" — differences among their
     * children are the diff's job, handled by the merge walk.
     */
    bool entriesDiffer(ScanNode const& localNode, ScanNode const& remoteNode);

    /**
     * @brief Parallel merge walk of two sorted scan trees.
     *
     * Assumes both @p local and @p remote have children sorted ascending by name
     * (as produced by @ref TreeDirectoryWalker). Emits to @p sink, respecting
     * @p options:
     *
     *  - one-sided subtrees emit exactly one node (no recursion),
     *  - `!options.recursive` suppresses both recursion and one-sided deletes
     *    whose @ref ScanNode::childrenKnown is false,
     *  - `options.ignoreHidden` filters per-name at every level.
     *
     * @see DiffSink::onProgress heartbeats fire every 512 compares.
     */
    void diffScanTrees(
        ScanNode const& local,
        ScanNode const& remote,
        DiffOptions const& options,
        DiffSink& sink
    );

    inline void to_json(nlohmann::json& j, DiffOptions const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, DiffOptions& s)
    {
        SharedData::from_json(j, s);
    }

    // Enum ADL bridges — see the matching comment in diff_tree_node.hpp for
    // why these are needed even though SharedData::to_json<EnumT> exists.
    inline void to_json(nlohmann::json& j, Direction const& e)
    {
        SharedData::to_json(j, e);
    }
    inline void from_json(nlohmann::json const& j, Direction& e)
    {
        SharedData::from_json(j, e);
    }
    inline void to_json(nlohmann::json& j, Action const& e)
    {
        SharedData::to_json(j, e);
    }
    inline void from_json(nlohmann::json const& j, Action& e)
    {
        SharedData::from_json(j, e);
    }

#ifdef NUI_FRONTEND
    // Mirror the to_json/from_json ADL bridges for the val-based serializer.
    // Sync-namespace enums aren't found by ADL through SharedData::to_val, so
    // we forward here.
    inline void to_val(Nui::val& v, Direction const& e)
    {
        SharedData::to_val(v, e);
    }
    inline void from_val(Nui::val const& v, Direction& e)
    {
        SharedData::from_val(v, e);
    }
    inline void to_val(Nui::val& v, Action const& e)
    {
        SharedData::to_val(v, e);
    }
    inline void from_val(Nui::val const& v, Action& e)
    {
        SharedData::from_val(v, e);
    }
    // Intentionally no to_val/from_val forwarder for DiffOptions — the
    // described-struct overload in nui's convertToVal picks it up automatically,
    // and defining ADL hooks here would make HasToVal<DiffOptions> ambiguous
    // against the generic path.
#endif
}
