#pragma once

#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/enqueue_plan_entry.hpp>
#include <shared_data/sync/scan_node.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SharedData::Sync
{
    /**
     * @brief The two scan trees a diff ran on, plus where they live. Non-owning.
     */
    struct ScanTrees
    {
        ScanNode const* local{nullptr};
        ScanNode const* remote{nullptr};
        std::filesystem::path localRoot{};
        std::filesystem::path remoteRoot{};
    };

    /**
     * @brief Resolves @p relKey against @p root by binary search over the sorted children.
     * @return nullptr when a segment does not exist.
     */
    ScanNode const* findScanNode(ScanNode const& root, std::string_view relKey);

    /** @brief True when @p action reads from the local side (upload, local delete). */
    bool actionSourceIsLocal(Action action);

    /**
     * @brief Rows of one diff section.
     *
     * Filled by @ref add during the merge walk, then @ref finalize synthesizes the structural ancestor
     * rows the tree needs above deep differences. One-sided directories are expanded lazily by
     * @ref loadChildren. Rows are stored once, keyed by relKey; child lists hold keys.
     */
    class DiffTreeStore
    {
      public:
        void clear(DiffOptions options = {});

        void add(DiffTreeNode node, std::string const& parentRelKey);

        /**
         * @brief Synthesizes structural ancestors, sorts every child list by name and fills child counts,
         *        subtree totals and the summary. Call once after the walk.
         */
        void finalize();

        SectionSummary const& summary() const
        {
            return summary_;
        }

        DiffTreeNode const* find(std::string const& relKey) const;

        /**
         * @brief Direct children of @p parentRelKey (empty = section root). Expands one-sided directories
         *        from the scan tree of their source side on first request and caches the rows.
         */
        std::vector<DiffTreeNode> loadChildren(std::string const& parentRelKey, ScanTrees const& trees);

        /**
         * @brief Turns a sparse selection (an entry implies its whole subtree) into transfers.
         *
         * Directories that are transferred as a whole are expanded into their files from the scan tree, so
         * ignore rules and hidden filters of the scan and diff hold for their content too. Directories without
         * scanned content, and every directory of a non-recursive diff, become directory-only entries. Deletes
         * stay whole directories.
         */
        std::vector<EnqueuePlanEntry>
        buildPlan(std::unordered_set<std::string> const& selectedRelKeys, ScanTrees const& trees) const;

        /** @brief Number of rows currently stored, structural and lazily loaded ones included. */
        std::size_t size() const
        {
            return nodes_.size();
        }

      private:
        void appendSubtreePlan(
            ScanNode const& directory,
            std::string const& relKey,
            Action action,
            ScanTrees const& trees,
            std::vector<EnqueuePlanEntry>& plan
        ) const;

        DiffOptions options_{};
        std::unordered_map<std::string, DiffTreeNode> nodes_{};
        std::unordered_map<std::string, std::vector<std::string>> children_{};
        /// Walk order of the emitted rows, then synthesized and lazily loaded rows.
        std::vector<std::string> order_{};
        SectionSummary summary_{};
    };

    /**
     * @brief DiffSink that fills an upload, a download and a delete @ref DiffTreeStore.
     */
    class StoreDiffSink final : public DiffSink
    {
      public:
        StoreDiffSink(
            DiffTreeStore& uploads,
            DiffTreeStore& downloads,
            DiffTreeStore& deletes,
            std::shared_ptr<std::atomic<bool>> cancelled = {},
            std::function<void(std::uint64_t)> onProgress = {}
        );

        void emitUpload(DiffTreeNode node, std::string const& parentRelKey) override;
        void emitDownload(DiffTreeNode node, std::string const& parentRelKey) override;
        void emitDelete(DiffTreeNode node, std::string const& parentRelKey) override;
        void onProgress(std::uint64_t entriesCompared) override;
        bool cancelled() const override;

        std::uint64_t lastCompared() const
        {
            return lastCompared_;
        }

      private:
        DiffTreeStore* uploads_;
        DiffTreeStore* downloads_;
        DiffTreeStore* deletes_;
        std::shared_ptr<std::atomic<bool>> cancelled_;
        std::function<void(std::uint64_t)> onProgress_;
        std::uint64_t lastCompared_{0};
    };
}
