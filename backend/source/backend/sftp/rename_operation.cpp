#include <backend/sftp/rename_operation.hpp>

#include <log/log.hpp>

RenameOperation::RenameOperation(SecureShell::SftpSession& sftp, RenameOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , sourcePath_{std::move(options.sourcePath)}
    , destinationPath_{std::move(options.destinationPath)}
    , futureTimeout_{options.futureTimeout}
{}

SecureShell::ProcessingStrand* RenameOperation::strand() const
{
    return sftp_->strand();
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
            auto fut = sftp_->rename(sourcePath_, destinationPath_);
            if (fut.wait_for(futureTimeout_) != std::future_status::ready)
                return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});

            const auto result = fut.get();
            if (!result.has_value())
                return enterErrorState<WorkStatus>({
                    .type = ErrorType::RenameFailure,
                    .sftpError = result.error(),
                    .extraInfo = sourcePath_.generic_string(),
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
        Log::info(
            "RenameOperation: Rename of '{}' to '{}' canceled.",
            sourcePath_.generic_string(),
            destinationPath_.generic_string()
        );
        state_ = OperationState::Canceled;
    }
    return {};
}
