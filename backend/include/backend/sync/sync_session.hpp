#pragma once

#include <ids/ids.hpp>
#include <paths/local_path.hpp>
#include <paths/relative_path.hpp>
#include <paths/remote_path.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_snapshot.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/diff_tree_store.hpp>
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
 *  3. @ref recomputeDiffInStrand invoked on every @c recomputeSyncDiff RPC refills the
 *     three @ref SharedData::Sync::DiffTreeStore sections, bumps @ref generation().
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
        Paths::LocalPath localRoot{};
        Paths::RemotePath remoteRoot{};
        /// Number of scan entries (sum of both sides) above which
        /// @ref SharedData::Sync::DiffSummary::heavyCompare is set to true.
        std::uint64_t heavyCompareThreshold{5000};
        /** @brief The local filesystem folds case, the platform default: Windows and macOS do, others do not. */
#if defined(_WIN32) || defined(__APPLE__)
        bool localFoldsCase{true};
#else
        bool localFoldsCase{false};
#endif
        /** @brief The server looks like it folds case, see RemoteDeleteGuardDetail::looksCaseFolding. */
        bool remoteFoldsCase{false};
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

    Paths::LocalPath const& localRoot() const noexcept
    {
        return options_.localRoot;
    }

    Paths::RemotePath const& remoteRoot() const noexcept
    {
        return options_.remoteRoot;
    }

    /**
     * @brief Where a row of the diff lives on both sides.
     */
    struct ResolvedPaths
    {
        Paths::LocalPath local;
        Paths::RemotePath remote;
    };

    /**
     * @brief The local and remote path of @p relative below the session roots. Fails for a name the local
     *        platform cannot store, nothing can leave the roots since @p relative holds no '..'.
     */
    std::expected<ResolvedPaths, Paths::PathError> resolve(Paths::RelativePath const& relative) const;

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
     * @brief @ref recomputeDiffInStrand plus the rows the dialog shows: every section root and the children
     *        of each parent in @p loadedParents that still exists. Precondition: running on @ref strand().
     */
    SharedData::Sync::DiffSnapshot recomputeSnapshotInStrand(
        SharedData::Sync::DiffOptions const& options,
        SharedData::Sync::LoadedParents const& loadedParents,
        std::function<void(std::uint64_t compared)> onProgress
    );

    /**
     * @brief Returns the direct children of @p parentRelKey in @p section (empty
     *        string = section root).  One-sided directories are expanded lazily from
     *        their scan tree.  Precondition: running on @ref strand().
     */
    std::vector<SharedData::Sync::DiffTreeNode> loadChildrenInStrand(
        SharedData::Sync::DiffSection section,
        std::string const& parentRelKey
    );

    struct ResolvedPlanEntry
    {
        SharedData::Sync::EnqueuePlanEntry entry;
        ResolvedPaths paths;
        /**
         * @brief What the compare saw at the path the entry replaces (the destination of a transfer) or deletes,
         *        which it may then only replace or delete while the path still holds it. A changed source is
         *        transferred as it is.
         */
        SharedData::ComparedState compared{};
    };

    struct ResolvedPlan
    {
        std::vector<ResolvedPlanEntry> entries{};
        /** @brief Keys of entries that have no path on one side, like a name Windows cannot store. */
        std::vector<std::string> refused{};
    };

    /**
     * @brief The plan of a sparse selection (an entry in @p selectedRelKeys selects it and everything below it, see
     *        @ref SharedData::Sync::DiffTreeStore::buildPlan) with every entry resolved below the session roots.
     *        Precondition: running on @ref strand().
     */
    ResolvedPlan resolvePlanInStrand(
        SharedData::Sync::DiffSection section,
        std::unordered_set<std::string> const& selectedRelKeys
    ) const;

  private:
    std::vector<SharedData::Sync::EnqueuePlanEntry> buildEnqueuePlanInStrand(
        SharedData::Sync::DiffSection section,
        std::unordered_set<std::string> const& selectedRelKeys
    ) const;
    SharedData::Sync::DiffTreeStore& storeFor(SharedData::Sync::DiffSection section);
    SharedData::Sync::DiffTreeStore const& storeFor(SharedData::Sync::DiffSection section) const;
    SharedData::Sync::ScanTrees scanTrees() const;

    Options options_;
    boost::asio::any_io_executor executor_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    std::shared_ptr<std::atomic<bool>> cancelled_;

    std::optional<SharedData::Sync::ScanNode> localTree_{};
    std::optional<SharedData::Sync::ScanNode> remoteTree_{};

    SharedData::Sync::DiffTreeStore uploads_{};
    SharedData::Sync::DiffTreeStore downloads_{};
    SharedData::Sync::DiffTreeStore deletes_{};

    std::uint64_t generation_{0};
};
