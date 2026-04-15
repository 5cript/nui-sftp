#pragma once

#include <ssh/sftp_error.hpp>
#include <utility/describe.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <ssh/async/processing_strand.hpp>
#include <log/log.hpp>

#include <ids/ids.hpp>

#include <optional>
#include <expected>

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

  protected:
    void enterState(OperationState newState)
    {
        state_ = newState;
    }

  protected:
    OperationState state_{OperationState::NotStarted};
    std::optional<Error> error_{std::nullopt};

  private:
    Ids::OperationId id_;
};