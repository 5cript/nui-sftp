#include <backend/sftp/rename_operation.hpp>

#include <log/log.hpp>
#include <remote_io/remote_file_system_helpers.hpp>

RenameOperation::RenameOperation(std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem, RenameOperationOptions options)
    : Operation{}
    , fileSystem_{std::move(fileSystem)}
    , sourcePath_{std::move(options.sourcePath)}
    , destinationPath_{std::move(options.destinationPath)}
    , futureTimeout_{options.futureTimeout}
{}

RemoteIo::ProcessingStrand* RenameOperation::strand() const
{
    return strandOf(fileSystem_);
}

std::expected<RenameOperation::WorkStatus, RenameOperation::Error> RenameOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case (NotStarted):
        case (Preparing):
        case (Prepared):
        {
            state_ = Running;
            [[fallthrough]];
        }
        case (Running):
        {
            const auto fileSystem = openSession(fileSystem_);
            if (!fileSystem)
                return enterErrorState<WorkStatus>(channelClosedError());
            // A queued rename mirrors a local move and must not replace what is at the destination.
            // The task outlives this operation when the wait times out.
            auto fut = fileSystem->performPromise(
                [session = fileSystem_,
                    source = sourcePath_,
                    destination = destinationPath_]() -> std::expected<void, RemoteIo::RemoteError>
                {
                    const auto fileSystem = session.lock();
                    if (!fileSystem)
                        return std::unexpected(RemoteIo::RemoteError{.message = "The SFTP channel was closed"});
                    return RemoteIo::renameNoReplaceInStrand(*fileSystem, source, destination);
                }
            );
            if (fut.wait_for(futureTimeout_) != std::future_status::ready)
                return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});

            const auto result = fut.get();
            if (!result.has_value())
                return enterErrorState<WorkStatus>({
                    .type = ErrorType::RenameFailure,
                    .remoteError = result.error(),
                    .extraInfo = sourcePath_.value(),
                });

            state_ = Completed;
            return WorkStatus::Complete;
        }
        case (Finalizing):
        {
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case (Completed):
        {
            Log::warn("RenameOperation: Operation already completed.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("RenameOperation: Operation already failed.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("RenameOperation: Operation was canceled.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("RenameOperation: Operation completed with partial success.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    Log::error("RenameOperation: Unknown operation state: {}", static_cast<int>(state_));
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, RenameOperation::Error> RenameOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info("RenameOperation: Rename of '{}' to '{}' canceled.", sourcePath_.value(), destinationPath_.value());
        state_ = OperationState::Canceled;
    }
    return {};
}
