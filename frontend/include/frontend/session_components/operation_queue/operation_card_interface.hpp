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

#include <nui/frontend/element_renderer.hpp>

#include <chrono>
#include <functional>
#include <optional>

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
    virtual void failedEntries(std::vector<std::pair<std::filesystem::path, SharedData::OperationError>> entries) = 0;

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