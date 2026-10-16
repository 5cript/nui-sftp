#pragma once

#include <ids/ids.hpp>
#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/scan_progress.hpp>

#include <nui/frontend/val.hpp>
#include <nui/rpc.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

class OperationQueue;

/**
 * @brief Shows the operations of the backend's LocalOperationQueue, which belong to no connection, in the operation
 *        queues of the session tabs.
 *
 * An operation shows in the queue that started it. When that queue goes away, with its tab or on a reconnect, the
 * card moves to the queue attached last, and without any queue to the next one that attaches. A delete that ends
 * while no queue is attached is reported as a notification.
 */
class LocalOperationRouter
{
  public:
    static LocalOperationRouter& instance();

    LocalOperationRouter(LocalOperationRouter const&) = delete;
    LocalOperationRouter& operator=(LocalOperationRouter const&) = delete;
    LocalOperationRouter(LocalOperationRouter&&) = delete;
    LocalOperationRouter& operator=(LocalOperationRouter&&) = delete;
    ~LocalOperationRouter() = default;

    /**
     * @brief @p queue can show cards from now on and takes the cards that have no queue.
     */
    void attach(OperationQueue& queue);

    /**
     * @brief @p queue goes away, its cards move to another queue.
     */
    void detach(OperationQueue& queue);

    /**
     * @brief The events of @p operationId go to @p queue. Call before the operation is queued.
     */
    void host(Ids::OperationId const& operationId, OperationQueue& queue);

    /**
     * @brief Drops @p operationId, which was not queued or whose card is gone.
     */
    void forget(Ids::OperationId const& operationId);

    /**
     * @brief Cancels every operation shown in @p queue and drops it, for a queue that clears its cards.
     */
    void cancelHostedBy(OperationQueue const& queue);

  private:
    LocalOperationRouter();

    struct Routed
    {
        OperationQueue* host{nullptr};
        std::optional<SharedData::OperationAdded> added{};
        std::optional<SharedData::ScanProgress> scanProgress{};
        std::optional<SharedData::BulkDeleteProgress> deleteProgress{};
    };

    void onOperationAdded(SharedData::OperationAdded const& added);
    void onScanProgress(SharedData::ScanProgress const& progress);
    void onDeleteProgress(SharedData::BulkDeleteProgress const& progress);
    void onOperationCompleted(Nui::val val);

    /**
     * @brief Shows the card of @p routed in @p queue as far as its operation got.
     */
    static void show(Routed& routed, OperationQueue& queue);

    /**
     * @brief The queue attached last, nullptr without any.
     */
    OperationQueue* newestQueue() const;

    std::vector<OperationQueue*> queues_{};
    std::unordered_map<Ids::OperationId, Routed, Ids::IdHash> routed_{};
    std::vector<Nui::RpcClient::AutoUnregister> registrations_{};
};
