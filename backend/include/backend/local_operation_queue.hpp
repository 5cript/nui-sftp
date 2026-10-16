#pragma once

#include <backend/rpc_helper.hpp>
#include <backend/sftp/local_delete_operation.hpp>
#include <ids/ids.hpp>
#include <paths/local_path.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operation_mode.hpp>

#include <nui/rpc.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Runs local deletes apart from the SSH sessions, so closing a tab or losing a connection never stops one.
 *
 * The deletes run one after another on the strand the queue shares with the sessions, a work step at a time, and
 * report to the frontend under "LocalOperationQueue::...". A delete a session started waits while the queue of that
 * session is paused, for as long as the session exists.
 */
class LocalOperationQueue
    : public RpcHelper::StrandRpc
    , public std::enable_shared_from_this<LocalOperationQueue>
{
  public:
    using OperationCompleted = SharedData::OperationCompleted;

    struct DeleteRequest
    {
        Ids::OperationId operationId{};
        std::vector<Paths::LocalPath> targets{};
        /** @brief See LocalDeleteOperation::LocalDeleteOperationOptions::recursive. */
        bool recursive{false};
        /** @brief The card refreshes the local listing when the delete ends. */
        bool insertRefresh{false};
        /** @brief A priority delete runs before the others and is never held. */
        SharedData::OperationMode mode{SharedData::OperationMode::Queued};
        /** @brief The session whose paused queue holds the delete, none for a delete without a session. */
        std::optional<Ids::SessionId> owner{};
        /** @brief The queue of @ref owner is paused, so the delete starts held. */
        bool held{false};
        /** @brief See LocalDeleteOperation::LocalDeleteOperationOptions::comparedStates. */
        std::vector<SharedData::ComparedState> comparedStates{};
    };

    LocalOperationQueue(
        boost::asio::any_io_executor executor,
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        std::shared_ptr<std::atomic_bool const> localDeletionPrevented
    );
    ~LocalOperationQueue() override = default;
    LocalOperationQueue(LocalOperationQueue const&) = delete;
    LocalOperationQueue& operator=(LocalOperationQueue const&) = delete;
    LocalOperationQueue(LocalOperationQueue&&) = delete;
    LocalOperationQueue& operator=(LocalOperationQueue&&) = delete;

    /**
     * @brief Handles "LocalOperationQueue::addDelete" (operation id, local paths), the delete of explorer items
     *        without a session, and "LocalOperationQueue::cancel" (operation id).
     */
    void registerRpc();

    /**
     * @brief Queues a delete with one card, labelled with the directory its targets lie in. Refused as a whole when
     *        one target is a root directory or contains a home directory. Assumed in strand.
     */
    std::expected<void, Operation::Error> addDelete(DeleteRequest request);

    /**
     * @brief Queues the delete of the explorer items @p pathsUtf8 with their content, as @ref addDelete with a card
     *        that refreshes the local listing. Refused when deletion is prevented in the settings or a path is not
     *        an item that may be deleted. Assumed in strand.
     *
     * @param owner The session whose explorer asks, none without one.
     * @param held The queue of @p owner is paused.
     * @return A message for Utility::localizedMessage or the language file saying why nothing was queued.
     */
    std::expected<void, std::string> addExplorerDelete(
        Ids::OperationId operationId,
        std::vector<std::string> const& pathsUtf8,
        std::optional<Ids::SessionId> owner,
        bool held
    );

    /**
     * @brief Holds the deletes of @p owner, or lets them run again. Assumed in strand.
     */
    void hold(Ids::SessionId const& owner, bool held);

    /**
     * @brief The deletes of @p owner run on their own from now on, never held again. Assumed in strand.
     */
    void release(Ids::SessionId const& owner);

    /**
     * @brief Ends the delete @p operationId as canceled and tells the frontend. Assumed in strand.
     */
    void cancel(Ids::OperationId const& operationId);

    /**
     * @brief Works the first delete that is not held for one step and tells the frontend when it ends. Assumed in
     *        strand; the queue drives itself, this is public for tests.
     *
     * @return True while a delete that is not held is left.
     */
    bool work();

    /**
     * @brief Test hook: @p observer sees every completion right before it is sent to the frontend.
     */
    void setCompletionObserver(std::function<void(OperationCompleted const&)> observer);

  private:
    struct QueuedDelete
    {
        Ids::OperationId operationId;
        std::unique_ptr<LocalDeleteOperation> operation;
        std::optional<Ids::SessionId> owner;
        bool held;
        bool priority;
    };

    /**
     * @brief Posts the next work step unless one is posted already.
     */
    void schedule();

    void complete(
        QueuedDelete const& queued,
        SharedData::OperationCompletionReason reason,
        std::optional<Operation::Error> error
    );
    std::string rpcName(std::string_view event) const;

    std::shared_ptr<std::atomic_bool const> localDeletionPrevented_;
    std::deque<QueuedDelete> deletes_{};
    bool scheduled_{false};
    std::function<void(OperationCompleted const&)> completionObserver_{};
};
