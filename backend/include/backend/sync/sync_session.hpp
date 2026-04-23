#pragma once

#include <ids/ids.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/enqueue_plan_entry.hpp>
#include <shared_data/sync/scan_node.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief Backend-resident holder for a single sync flow.
 *
 * Lifecycle (driven from @ref OperationQueue RPC handlers):
 *  1. Constructed when the frontend calls @c openSyncSession.
 *  2. @ref setLocalTreeInStrand / @ref setRemoteTreeInStrand called once each by the
 *     scan-completion callbacks.
 *  3. @ref recomputeDiffInStrand invoked on every @c recomputeSyncDiff RPC — fills the
 *     three @ref DiffTreeStore adjacency maps in-place, bumps @ref generation().
 *  4. @ref loadChildrenInStrand serves lazy subtree slices to the frontend tree.
 *  5. Destroyed when @c closeSyncSession fires or the owning channel dies.
 *
 * Threading contract: every mutation and read method requires the caller to post onto
 * @ref strand() first.  Owned state (scan trees, diff stores, cancel flag) is never
 * touched from any other thread, so no internal locks are needed.
 *
 * Cancellation: a captured @ref std::shared_ptr to an @ref std::atomic_bool lets
 * in-flight diff walks exit early.  @ref cancel() is lock-free and safe from any
 * thread; it bumps the flag but does NOT post onto the strand — the running walk
 * observes it at the next checkpoint.
 */
class SyncSession : public std::enable_shared_from_this<SyncSession>
{
  public:
    struct Options
    {
        Ids::SyncSessionId sessionId{};
        std::filesystem::path localRoot{};
        std::filesystem::path remoteRoot{};
        /// Number of scan entries (sum of both sides) above which
        /// @ref SharedData::Sync::DiffSummary::heavyCompare is set to true.
        std::uint64_t heavyCompareThreshold{5000};
    };

    SyncSession(Options options, boost::asio::any_io_executor executor);
    ~SyncSession();
    SyncSession(SyncSession const&) = delete;
    SyncSession& operator=(SyncSession const&) = delete;
    SyncSession(SyncSession&&) = delete;
    SyncSession& operator=(SyncSession&&) = delete;

    Ids::SyncSessionId id() const
    {
        return options_.sessionId;
    }
    std::uint64_t generation() const
    {
        return generation_;
    }

    boost::asio::strand<boost::asio::any_io_executor>& strand()
    {
        return strand_;
    }

    /**
     * @brief Thread-safe cancel flag bump.  The running walk sees it at the next
     *        per-512-compare checkpoint.  Does not post onto the strand.
     */
    void cancel();

    /** @brief Whether the cancel flag is currently set. */
    bool isCancelled() const;

    /**
     * @brief Assumes ownership of a fully-built scan tree. Precondition: running on
     *        @ref strand().
     */
    void setLocalTreeInStrand(SharedData::Sync::ScanNode tree);
    void setRemoteTreeInStrand(SharedData::Sync::ScanNode tree);

    /** @brief True once both scan trees have been handed in. */
    bool bothTreesReadyInStrand() const;

    /**
     * @brief Runs the parallel merge walk, populating the three diff-tree stores, and
     *        returns a summary. Precondition: running on @ref strand().
     *
     * @param options    Diff options from the frontend settings.
     * @param onProgress Invoked from the strand every 512 compares (then once more with
     *                   the final count). Implementations typically hop to
     *                   @c wnd_->runInJavascriptThread and emit an onSyncDiffProgress
     *                   RPC from there.
     */
    SharedData::Sync::DiffSummary recomputeDiffInStrand(
        SharedData::Sync::DiffOptions const& options,
        std::function<void(std::uint64_t compared)> onProgress
    );

    /**
     * @brief Returns the direct children of @p parentRelKey in @p section (empty
     *        string = section root).  For a one-sided directory that has not yet
     *        been explored, lazily walks the corresponding ScanNode subtree and
     *        caches the resulting DiffTreeNodes into the section store so future
     *        calls are O(1).  Precondition: running on @ref strand().
     */
    std::vector<SharedData::Sync::DiffTreeNode> loadChildrenInStrand(
        SharedData::Sync::DiffSection section,
        std::string const& parentRelKey
    );

    /**
     * @brief Computes the minimal set of enqueue plan entries from a SPARSE
     *        selection set: an entry X in @p selectedRelKeys means "X and every
     *        descendant is selected".  See @ref SharedData::Sync::minimizeEnqueueIndices
     *        for the emission rules (bulk dirs collapse; structural dirs expand
     *        to leaves).  Precondition: running on @ref strand().
     */
    std::vector<SharedData::Sync::EnqueuePlanEntry> buildEnqueuePlanInStrand(
        SharedData::Sync::DiffSection section,
        std::unordered_set<std::string> const& selectedRelKeys
    ) const;

  private:
    /**
     * @brief Per-section result store.
     *        - @ref childrenByParent maps parentRelKey -> sorted DiffTreeNode list
     *          (the sort order is the one the merge walk used).
     *        - @ref emissionOrder keeps the full emission list in walk order.  Used
     *          by @ref buildEnqueuePlanInStrand to preserve deterministic plan order
     *          and to run the selection-minimization over a stable sequence.
     */
    struct DiffTreeStore
    {
        std::unordered_map<std::string, SharedData::Sync::DiffTreeNode> nodesByRelKey{};
        std::unordered_map<std::string, std::vector<SharedData::Sync::DiffTreeNode>> childrenByParent{};
        std::vector<std::string> emissionOrder{};
        SharedData::Sync::SectionSummary summary{};

        void clear();
    };

    Options options_;
    boost::asio::any_io_executor executor_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    std::shared_ptr<std::atomic<bool>> cancelled_;

    std::optional<SharedData::Sync::ScanNode> localTree_{};
    std::optional<SharedData::Sync::ScanNode> remoteTree_{};

    DiffTreeStore uploads_{};
    DiffTreeStore downloads_{};
    DiffTreeStore deletes_{};

    std::uint64_t generation_{0};
};
