#pragma once

#include <ids/ids.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_snapshot.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/enqueue_plan_entry.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class OperationQueue;

/**
 * @brief Frontend handle to a backend-resident SyncSession.
 *
 * Owns the @ref Ids::SyncSessionId and wraps the RPC call surface:
 *
 *  - @ref open queues both scans, bridges @c onSyncScanPhaseDone to @p onBothListed.
 *  - @ref recompute triggers a backend diff walk and returns the summary plus the rows to show.
 *    The stored @ref generation_ is advanced; stale @ref loadChildren responses
 *    issued against a previous generation are silently dropped.
 *  - @ref loadChildren feeds the frontend's lazy @ref Tree.
 *  - @ref cancelDiff flips the backend cancel flag.
 *  - @ref close releases backend memory.  Idempotent.
 *
 * The provider owns the lifecycle — callers should construct it once per sync flow
 * and destroy it when the dialog closes.  The destructor calls @ref close, replies that
 * arrive afterwards are dropped.
 */
class BackendSyncProvider
{
  public:
    explicit BackendSyncProvider(OperationQueue* queue);
    ~BackendSyncProvider();
    BackendSyncProvider(BackendSyncProvider const&) = delete;
    BackendSyncProvider& operator=(BackendSyncProvider const&) = delete;
    BackendSyncProvider(BackendSyncProvider&&) = delete;
    BackendSyncProvider& operator=(BackendSyncProvider&&) = delete;

    Ids::SyncSessionId sessionId() const
    {
        return sessionId_;
    }
    std::uint64_t generation() const
    {
        return generation_;
    }

    /**
     * @brief Kicks off both scans.  @p onBothListed fires after both sides have
     *        reported phase-done (the order between them isn't significant).
     */
    void open(
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        std::function<void(SharedData::ScanProgress const&)> onLocalListing,
        std::function<void(SharedData::ScanProgress const&)> onRemoteListing,
        std::function<void()> onBothListed,
        std::function<void(std::uint64_t compared)> onDiffProgress
    );

    /**
     * @brief Runs a backend diff with @p options. The snapshot holds the summary with the new generation,
     *        the section roots and the children of every still existing parent in @p loadedParents.
     */
    void recompute(
        SharedData::Sync::DiffOptions options,
        SharedData::Sync::LoadedParents loadedParents,
        std::function<void(SharedData::Sync::DiffSnapshot)> onSnapshot
    );

    /**
     * @brief Lazy loader for the @ref ScriptNuiComponents::Tree.  Rejections include
     *        stale-generation responses; callers should silently drop those.
     */
    void loadChildren(
        SharedData::Sync::DiffSection section,
        std::string const& parentRelKey,
        std::function<void(std::vector<SharedData::Sync::DiffTreeNode>)> onResolved,
        std::function<void(std::string const&)> onRejected
    );

    /**
     * @brief Asks the backend to collapse @p selectedRelKeys into a minimal
     *        enqueue plan for @p section.  Uses the cached @ref generation_ so
     *        stale requests (after a recompute) are rejected by the server.
     */
    void buildEnqueuePlan(
        SharedData::Sync::DiffSection section,
        std::vector<std::string> selectedRelKeys,
        std::function<void(std::vector<SharedData::Sync::EnqueuePlanEntry>)> onResolved,
        std::function<void(std::string const&)> onRejected
    );

    /** @brief Flips the backend cancel flag; an in-flight diff walk bails at next checkpoint. */
    void cancelDiff();

    /** @brief Releases backend-side memory. Idempotent. */
    void close();

  private:
    OperationQueue* queue_;
    Ids::SyncSessionId sessionId_;
    std::uint64_t generation_{0};
    bool closed_{false};
    /// Expires with the provider so late RPC replies never touch a destroyed instance.
    std::shared_ptr<BackendSyncProvider*> alive_{std::make_shared<BackendSyncProvider*>(this)};
};
