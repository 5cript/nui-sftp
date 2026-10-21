#pragma once

#include <ssh/sftp_error.hpp>
#include <utility/describe.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <ssh/async/processing_strand.hpp>
#include <ssh/async/transfer_context.hpp>
#include <ssh/sftp_session.hpp>
#include <log/log.hpp>

#include <ids/ids.hpp>

#include <libssh/sftp.h>

#include <expected>
#include <memory>
#include <optional>
#include <system_error>

class Operation
{
  public:
    Operation()
        : id_{Ids::generateOperationId()}
    {}
    Operation(Operation const&) = delete;
    Operation& operator=(Operation const&) = delete;
    Operation(Operation&&) = delete;
    Operation& operator=(Operation&&) = delete;

    virtual ~Operation() = default;

    using ErrorType = SharedData::OperationErrorType;
    using Error = SharedData::OperationError;

    virtual SharedData::OperationType type() const = 0;

    template <typename FunctionT>
    auto visit(FunctionT&& func) const;

    virtual SecureShell::ProcessingStrand* strand() const = 0;

    virtual void pause(bool)
    {
        /* noop */
    }

    template <typename FunctionT>
    bool perform(FunctionT&& func)
    {
        if (auto* theStrand = strand(); theStrand)
            return theStrand->pushTask(std::forward<FunctionT>(func));
        else
        {
            Log::error("Operation: Cannot perform task on strand, no processing strand available.");
            return false;
        }
    }

    Ids::OperationId id() const
    {
        return id_;
    }

    using OperationState = SharedData::OperationState;

    OperationState state() const
    {
        return state_;
    }

    /**
     * @brief Can parallel actions go beyond this operation?
     *
     * @return true Cannot progress beyond this operation.
     * @return false Can progress beyond this operation.
     */
    virtual bool isBarrier() const noexcept = 0;

    /**
     * @brief How much parallel work does this operation do.
     *
     * @param parallel Maximum parallelism allowed.
     * @return The amount of parallel work that can be done maxed by parallel parameter.
     */
    virtual int parallelWorkDoable(int parallel) const noexcept = 0;

    /**
     * @brief Tells the operation how many transfer slots the queue granted it for the next work step.
     *        Only operations that answer more than 1 in @ref parallelWorkDoable care.
     *
     * @param slots Granted slots, at least 1.
     */
    virtual void setParallelBudget(int)
    {
        /* noop */
    }

    enum class WorkStatus
    {
        MoreWork,
        Complete
    };

    /**
     * @brief Performs work for the operation depending on the operation type.
     *
     * Call site is free to run this from any thread — implementations that need the SFTP
     * processing strand must push into it themselves. Prefer calling @ref workInStrand from
     * @ref OperationQueue so that several operations share a single strand umbrella.
     *
     * @return std::expected<bool, Error>, true if it wants to be retriggered without delay.
     */
    virtual std::expected<WorkStatus, Error> work() = 0;

    /**
     * @brief In-strand variant of @ref work. Runs one step of the state machine assuming the
     * caller has already hopped into the SFTP processing strand.
     *
     * Default implementation forwards to @ref work so legacy operations keep working — only
     * operations that have been converted to SFTP *InStrand primitives should override this.
     *
     * @return std::expected<WorkStatus, Error>
     */
    virtual std::expected<WorkStatus, Error> workInStrand()
    {
        return work();
    }

    /**
     * @brief Whether this operation needs the SFTP processing strand to make progress.
     *
     * Ops that return false (e.g. pure local filesystem scans) are driven directly on the
     * queue's caller thread and skipped when the queue builds its batched strand umbrella.
     *
     * @return true if @ref workInStrand must run on the SFTP processing strand.
     */
    virtual bool usesStrand() const noexcept
    {
        return true;
    }

    /**
     * @brief Cancels the operation.
     *
     * @return std::expected<void, Error>
     */
    virtual std::expected<void, Error> cancel(bool adoptCancelState) = 0;

    template <typename T = void>
    std::expected<T, Error> enterErrorState(Error error)
    {
        state_ = OperationState::Failed;
        error_ = std::move(error);
        const auto cancelResult = cancel(false);
        if (!cancelResult.has_value())
        {
            Log::error("Operation: Failed to cancel operation: {}", cancelResult.error().toString());
            // If cancel fails, we still want to call the completion callback.
        }
        return std::unexpected(error_.value());
    }

  public:
    /**
     * @brief The error of a finished transfer that could not be moved into place on the server: FileExists when the
     *        destination exists and may not be replaced.
     */
    static Error remoteMoveError(SecureShell::SftpError error)
    {
        if (error.sftpError == SSH_FX_FILE_ALREADY_EXISTS)
            return Error{.type = ErrorType::FileExists};
        return Error{.type = ErrorType::SftpError, .sftpError = std::move(error)};
    }

    /**
     * @brief The error of a finished transfer that could not be moved into place on this machine: FileExists when the
     *        destination exists and may not be replaced.
     */
    static Error localMoveError(std::error_code error)
    {
        if (error == std::errc::file_exists)
            return Error{.type = ErrorType::FileExists};
        return Error{.type = ErrorType::RenameFailure};
    }

    /**
     * @brief The level a failure with @p error is logged at: a destination that changed since its compare is no error,
     *        the compare kept it on purpose and the owner of the operation reports it.
     */
    static Log::Level failureLevelOf(Error const& error)
    {
        return error.type == ErrorType::ChangedSinceCompare ? Log::Level::Warning : Log::Level::Error;
    }

  protected:
    void enterState(OperationState newState)
    {
        state_ = newState;
    }

    /**
     * @brief The operation completed, failed or was canceled.
     */
    bool hasEnded() const noexcept
    {
        return state_ >= OperationState::Completed;
    }

    /**
     * @brief The session behind @p session for one work step, none once its channel was closed or is closing.
     *        Operations hold their session weakly and lock it per step, so a closed channel ends them instead of
     *        leaving them a dangling session.
     */
    static std::shared_ptr<SecureShell::SftpSession> openSession(std::weak_ptr<SecureShell::SftpSession> const& session)
    {
        auto locked = session.lock();
        if (!locked || !locked->strand() || locked->strand()->isFinalized())
            return nullptr;
        return locked;
    }

    /**
     * @brief The processing strand of @p session, none once its channel was closed or is closing. It stays valid
     *        until the channel is closed, which happens on the queue's strand.
     */
    static SecureShell::ProcessingStrand* strandOf(std::weak_ptr<SecureShell::SftpSession> const& session)
    {
        const auto locked = openSession(session);
        return locked ? locked->strand() : nullptr;
    }

    /**
     * @brief The error of an operation whose SFTP channel was closed before it ended.
     */
    static Error channelClosedError()
    {
        return Error{.type = ErrorType::ChannelClosed, .extraInfo = "The SFTP channel was closed"};
    }

    /**
     * @brief Waits until every task queued on @p strand so far has run, so none of them outlives the operation that
     *        queued it. Returns at once on the processing thread itself and on a finalized strand.
     */
    static void waitForStrandTasks(SecureShell::ProcessingStrand* strand)
    {
        if (strand && !strand->withinProcessingThread())
            strand->pushPromiseTask([]() {}).wait();
    }

    /**
     * @brief The error of an async transfer that ended without completing.
     */
    static Error asyncTransferError(SecureShell::AsyncTransferContext const& context)
    {
        if (auto error = context.error(); error.has_value())
            return Error{.type = ErrorType::SftpError, .sftpError = std::move(error)};
        return Error{.type = ErrorType::SftpError, .extraInfo = "The transfer ended before it completed"};
    }

  protected:
    OperationState state_{OperationState::NotStarted};
    std::optional<Error> error_{std::nullopt};

  private:
    Ids::OperationId id_;
};