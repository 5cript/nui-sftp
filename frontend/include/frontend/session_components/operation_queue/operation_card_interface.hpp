#pragma once

#include <frontend/resumable_op.hpp>

#include <shared_data/file_operations/transfer_progress.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/is_paused.hpp>
#include <shared_data/error_or_success.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

class OperationCardInterface
{
  public:
    virtual ~OperationCardInterface() = default;

    virtual bool warrantsCancelConfirm() const = 0;
    virtual Nui::ElementRenderer body() const = 0;
    virtual void state(SharedData::OperationState newState) = 0;
    virtual SharedData::OperationState state() const = 0;
    virtual SharedData::OperationType type() const = 0;
    virtual bool isCompletedState() const = 0;
    virtual std::chrono::steady_clock::time_point completionTime() const = 0;
    virtual Nui::ElementRenderer operator()() const = 0;
    virtual void setError(SharedData::OperationError const& error) = 0;
    virtual void failedEntries(std::vector<std::pair<std::string, SharedData::OperationError>> entries) = 0;
    virtual Ids::OperationId operationId() const = 0;

    /**
     * @brief What the cancel options of a transfer card call, installed by the queue like setKickToTopHandler.
     */
    struct PartialFileHandlers
    {
        /** @brief Opens the cancel menu next to the element with the given id. */
        std::function<void(std::string const& anchorId)> openCancelMenu{};
        std::function<void()> continueTransfer{};
        std::function<void()> discardPartialFiles{};
    };
    virtual void setPartialFileHandlers(PartialFileHandlers handlers) = 0;

    /**
     * @brief Whether a cancel deletes the partial files of the transfer, unknown for other operations. A card that
     *        deletes them offers to keep them too.
     */
    virtual void deletesPartialFilesOnCancel(std::optional<bool> deletes) = 0;
    virtual std::optional<bool> deletesPartialFilesOnCancel() const = 0;

    /**
     * @brief A cancel deletes the partial files of the transfer and can keep them instead: it is no archive, which
     *        nothing continues.
     */
    virtual bool offersToKeepPartialFiles() const = 0;

    /**
     * @brief Whether a cancel keeps the partial files, asked to keep them when @p keepRequested is set.
     */
    virtual bool keepsPartialFilesOnCancel(bool keepRequested) const = 0;

    /**
     * @brief The transfer was canceled keeping its partial files, the card offers to continue or discard them.
     */
    virtual void keptPartialFiles(bool kept) = 0;
    virtual bool keptPartialFiles() const = 0;

    /**
     * @brief The progress and the maximum of the bar of a transfer.
     */
    virtual std::pair<std::int64_t, std::int64_t> barPosition() const
    {
        return {0, 0};
    }

    /**
     * @brief Puts the bar of a transfer where the card it replaces stopped.
     */
    virtual void barPosition(std::int64_t /*progress*/, std::int64_t /*maximum*/)
    {
        /* noop */
    }

    /**
     * @brief Install a click handler for the kick-to-top affordance.  The
     *        OperationQueue calls this after the card is constructed so the
     *        button can directly trigger a `moveOperation(id, 0)` RPC
     *        without coupling the card to the queue.  Empty handler =
     *        button is rendered but is a no-op (and is hidden by CSS unless
     *        the queue root has `data-paused`).
     */
    virtual void setKickToTopHandler(std::function<void()> handler) = 0;

    /**
     * @brief Describe this operation so it can be re-enqueued (with
     *        tryContinue=true) after a seamless reconnect.  Scan/sync
     *        operations return std::nullopt and are dropped from the snapshot.
     *        Completed / failed / canceled cards should also return nullopt
     *        — only in-flight work needs resuming.
     */
    virtual std::optional<ResumableOp> resumableDescriptor() const
    {
        return std::nullopt;
    }
};