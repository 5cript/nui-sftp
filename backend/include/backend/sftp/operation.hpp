#pragma once

#include <utility/describe.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <remote_io/async/processing_strand.hpp>
#include <remote_io/async/transfer_context.hpp>
#include <remote_io/remote_file_system.hpp>
#include <log/log.hpp>
#include <paths/remote_path.hpp>

#include <atomic>
#include <concepts>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

template <>
struct RemoteIo::UnfinishedTaskResult<SharedData::OperationError>
{
    SharedData::OperationError operator()() const
    {
        return SharedData::OperationError{
            .type = SharedData::OperationErrorType::ChannelClosed,
            .extraInfo = "The task did not finish, the SFTP session is closed or its thread stopped",
        };
    }
};

class Operation
{
  public:
    Operation() = default;
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

    virtual RemoteIo::ProcessingStrand* strand() const = 0;

    virtual void pause(bool)
    {
        /* noop */
    }

    /**
     * @brief The temporary files a canceled transfer left for a later transfer to continue.
     */
    struct KeptPartialFiles
    {
        std::vector<std::filesystem::path> local{};
        std::vector<Paths::RemotePath> remote{};
    };

    /**
     * @brief The next cancel keeps the partial files, whatever the options say. Called before cancel(true).
     */
    virtual void keepPartialFiles()
    {
        /* noop */
    }

    /**
     * @brief What a cancel leaves to continue, nothing for an operation that deletes its partial files on cancel or
     *        transfers no files.
     */
    virtual std::optional<KeptPartialFiles> keptPartialFiles() const
    {
        return std::nullopt;
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
        /** @brief Work again right away. */
        MoreWork,
        Complete,
        /** @brief Nothing to do until the operation's wake is called, by a transfer that progressed or ended. */
        Waiting,
    };

    /**
     * @brief Sets what the operation calls when it has work again after answering Waiting. Callable from any thread.
     */
    void setWake(std::function<void()> wake)
    {
        wake_ = std::move(wake);
    }

    bool hasWake() const noexcept
    {
        return static_cast<bool>(wake_);
    }

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
    static Error remoteMoveError(RemoteIo::RemoteError error)
    {
        if (error.kind == RemoteIo::RemoteErrorKind::AlreadyExists)
            return Error{.type = ErrorType::FileExists};
        return Error{.type = ErrorType::SftpError, .remoteError = std::move(error)};
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
     *        the compare kept it on purpose and the owner of the operation reports it, an open that waits for a
     *        connection no failure at all.
     */
    static Log::Level failureLevelOf(Error const& error)
    {
        if (waitsForConnection(error))
            return Log::Level::Debug;
        return error.type == ErrorType::ChangedSinceCompare ? Log::Level::Warning : Log::Level::Error;
    }

  protected:
    /**
     * @brief Lets @p context wake the queue when it progresses or ends.
     */
    void observeTransfer(RemoteIo::AsyncTransferContext& context) const
    {
        context.observe(wake_);
    }

    /**
     * @brief An open of @p path in @p mode that wakes the queue after it failed with Busy, see waitsForConnection.
     */
    RemoteIo::OpenFile wakingOpen(
        Paths::RemotePath path,
        RemoteIo::OpenMode mode,
        std::filesystem::perms permissions = RemoteIo::OpenFile{}.permissions
    )
    {
        connectionFreed_ = std::make_shared<std::atomic_bool>(false);
        awaitsConnection_ = false;
        return RemoteIo::OpenFile{
            .path = std::move(path),
            .mode = mode,
            .permissions = permissions,
            .wakeWhenFree = [freed = connectionFreed_, wake = wake_]()
            {
                freed->store(true);
                if (wake)
                    wake();
            },
        };
    }

    /**
     * @brief Whether @p error is an open that found every connection taken: the operation answers Waiting and opens
     *        again once the file system woke it, an open from wakingOpen passed the wake.
     */
    static bool waitsForConnection(Error const& error)
    {
        return error.remoteError.has_value() && error.remoteError->kind == RemoteIo::RemoteErrorKind::Busy;
    }

    /**
     * @brief Waits for the connection the latest wakingOpen found taken: awaitsConnection holds until its wake was
     *        called.
     */
    WorkStatus awaitConnection()
    {
        awaitsConnection_ = true;
        return WorkStatus::Waiting;
    }

    /**
     * @brief Whether the operation still waits for a connection, see awaitConnection. A work step meanwhile answers
     *        Waiting without opening again: the queue works every operation whenever another one progressed.
     */
    bool awaitsConnection() const
    {
        return awaitsConnection_ && connectionFreed_ && !connectionFreed_->load();
    }

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
     * @brief The file system behind @p fileSystem for one work step, none once its channel was closed or is closing.
     *        Operations hold their file system weakly and lock it per step, so a closed channel ends them instead of
     *        leaving them a dangling file system. Keeps the type an operation holds, which may be a protocol's own.
     */
    template <std::derived_from<RemoteIo::IRemoteFileSystem> FileSystemT>
    static std::shared_ptr<FileSystemT> openSession(std::weak_ptr<FileSystemT> const& fileSystem)
    {
        auto locked = fileSystem.lock();
        if (!locked || !locked->strand() || locked->strand()->isFinalized())
            return nullptr;
        return locked;
    }

    /**
     * @brief The processing strand of @p fileSystem, none once its channel was closed or is closing. It stays valid
     *        until the channel is closed, which happens on the queue's strand.
     */
    static RemoteIo::ProcessingStrand* strandOf(std::weak_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem)
    {
        const auto locked = openSession(fileSystem);
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
    static void waitForStrandTasks(RemoteIo::ProcessingStrand* strand)
    {
        if (strand && !strand->withinProcessingThread())
            strand->pushPromiseTask([]() {}).wait();
    }

    /**
     * @brief Waits until the server confirmed the transfer through @p stream, then closes it. A transfer it fails for
     *        must not be used, even when every byte moved.
     */
    static std::expected<void, Error> finishTransferInStrand(std::weak_ptr<RemoteIo::IRemoteFileStream> const& stream)
    {
        const auto locked = stream.lock();
        if (!locked)
            return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
        const auto finished = locked->finishInStrand();
        locked->closeInStrand();
        if (!finished)
            return std::unexpected(Error{.type = ErrorType::SftpError, .remoteError = finished.error()});
        return {};
    }

    /**
     * @brief The error of an async transfer that ended without completing.
     */
    static Error asyncTransferError(RemoteIo::AsyncTransferContext const& context)
    {
        if (auto error = context.error(); error.has_value())
            return Error{.type = ErrorType::SftpError, .remoteError = std::move(error)};
        return Error{.type = ErrorType::SftpError, .extraInfo = "The transfer ended before it completed"};
    }

  protected:
    OperationState state_{OperationState::NotStarted};
    std::optional<Error> error_{std::nullopt};
    /** @brief Set by the queue before the first work step, see setWake. */
    std::function<void()> wake_{};

  private:
    /** @brief Set by the wake of the latest wakingOpen, from any thread. */
    std::shared_ptr<std::atomic_bool> connectionFreed_{};
    /** @brief The latest wakingOpen failed with Busy, see awaitConnection. */
    bool awaitsConnection_{false};
};