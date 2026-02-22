#include <backend/sftp/scan_operation.hpp>
#include <log/log.hpp>

#include <ssh/sftp_session.hpp>

ScanOperation::ScanOperation(SecureShell::SftpSession& sftp, ScanOperationOptions options)
    : sftp_(&sftp)
    , remotePath_{std::move(options.remotePath)}
    , progressCallback_{std::move(options.progressCallback)}
    , futureTimeout_{options.futureTimeout}
{}

ScanOperation::~ScanOperation() = default;

std::expected<std::vector<SharedData::DirectoryEntry>, ScanOperation::Error>
ScanOperation::scanner(std::filesystem::path const& path)
{
    auto fut = sftp_->listDirectory(path);
    fut.wait_for(futureTimeout_);
    if (fut.wait_for(futureTimeout_) != std::future_status::ready)
        return enterErrorState<std::vector<SharedData::DirectoryEntry>>({.type = ErrorType::FutureTimeout});

    auto result = fut.get();
    if (!result.has_value())
        return enterErrorState<std::vector<SharedData::DirectoryEntry>>(
            {.type = ErrorType::SftpError, .sftpError = result.error()}
        );

    return {std::move(result).value()};
}

std::uint64_t ScanOperation::totalBytes() const
{
    return walker_->totalBytes();
}

std::expected<ScanOperation::WorkStatus, ScanOperation::Error> ScanOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case (NotStarted):
        {
            state_ = Running;
            Log::info("ScanOperation: Starting scan of '{}'.", remotePath_.generic_string());
            progressCallback_(0, 0, 0);
            return WorkStatus::MoreWork;
        }
        case (Running):
        {
            return withWalkerDo(
                [this](auto& walker) -> std::expected<ScanOperation::WorkStatus, ScanOperation::Error>
                {
                    if (walker.completed())
                    {
                        Log::info("ScanOperation: Scan of '{}' completed.", remotePath_.generic_string());
                        state_ = Completed;
                        return WorkStatus::Complete;
                    }

                    auto result = walker.walk();
                    if (!result.has_value())
                    {
                        Log::error("ScanOperation: Failed to scan directory: {}", result.error().toString());
                        return enterErrorState<WorkStatus>(result.error());
                    }
                    // -1, because the walker includes the base/root dir of the search:
                    progressCallback_(walker.totalBytes(), walker.currentIndex(), walker.totalEntries() - 1);
                    return WorkStatus::MoreWork;
                }
            );
        }
        case (Prepared):
        case (Preparing):
        case (Finalizing):
            Log::error("ScanOperation: Invalid state: {}", static_cast<int>(state_));
            return enterErrorState<WorkStatus>({.type = ErrorType::InvalidOperationState});
        case (Completed):
        {
            Log::warn("ScanOperation: Operation already completed.");
            // Dont enter error state here, it would overwrite the success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("ScanOperation: Operation already failed.");
            // Do not enter error state here, it would overwrite the error state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("ScanOperation: Cannot work on canceled operation.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("ScanOperation: Operation completed with partial success.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, ScanOperation::Error> ScanOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info("ScanOperation: Scan of '{}' canceled.", remotePath_.generic_string());
        enterState(OperationState::Canceled);
    }
    return {};
}

SecureShell::ProcessingStrand* ScanOperation::strand() const
{
    return sftp_->strand();
}