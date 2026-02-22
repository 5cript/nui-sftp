#include <backend/sftp/delete_operation.hpp>

#include <ssh/sftp_session.hpp>

DeleteOperation::DeleteOperation(SecureShell::SftpSession& sftp, DeleteOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , remotePath_{std::move(options.remotePath)}
    , filesRemovedProgress_{std::move(options.filesRemovedProgress)}
    , futureTimeout_{options.futureTimeout}
    , recursive_{options.recursive}
{}
DeleteOperation::~DeleteOperation()
{}

SecureShell::ProcessingStrand* DeleteOperation::strand() const
{
    return sftp_->strand();
}

std::expected<DeleteOperation::WorkStatus, DeleteOperation::Error> DeleteOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case (NotStarted):
        {
            state_ = Preparing;
            [[fallthrough]];
        }
        case (Preparing):
        {
            if (!recursive_)
            {
                totalToDelete_ = 1;
                filesRemovedProgress_ = {};
            }

            state_ = Prepared;
            [[fallthrough]];
        }
        case (Prepared):
        {
            state_ = Running;
            [[fallthrough]];
        }
        case (Running):
        {
            if (deletedThusFar_ == totalToDelete_)
            {
                state_ = Completed;
                return WorkStatus::Complete;
            }
            const auto currentPath = [this]()
            {
                if (!recursive_)
                    return remotePath_;
                // go from back:
                return fullPath(entries_, entries_[totalToDelete_ - deletedThusFar_ - 1]);
            }();
            auto removeResult = removeOnce(currentPath);
            if (!removeResult.has_value())
                return std::unexpected(removeResult.error());
            if (deletedThusFar_ == totalToDelete_)
            {
                state_ = Completed;
                return WorkStatus::Complete;
            }
            return WorkStatus::MoreWork;
        }
        case (Finalizing):
        {
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case (Completed):
        {
            Log::warn("DeleteOperation: Operation already completed.");
            // Dont enter error state here, it would overwrite the success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("DeleteOperation: Operation already failed.");
            // Do not enter error state here, it would overwrite the error state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("DeleteOperation: Operation was canceled.");
            // Do not enter error state here, it would overwrite the cancel state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("DeleteOperation: Operation completed with partial success.");
            // Do not enter error state here, it would overwrite the partial success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    Log::error("DeleteOperation: Unknown operation state: {}", static_cast<int>(state_));
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, DeleteOperation::Error> DeleteOperation::removeOnce(std::filesystem::path const& path)
{
    auto fut = sftp_->removeAll({path});
    if (fut.wait_for(futureTimeout_) != std::future_status::ready)
        return enterErrorState<void>({.type = ErrorType::FutureTimeout});

    const auto result = fut.get();
    if (!result.has_value())
        return enterErrorState<void>(
            {.type = ErrorType::DeleteFailed, .sftpError = result.error(), .extraInfo = path.generic_string()}
        );

    ++deletedThusFar_;
    if (filesRemovedProgress_)
        filesRemovedProgress_(path.generic_string(), deletedThusFar_, totalToDelete_);
    return {};
}

void DeleteOperation::setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t /* totalBytes */)
{
    entries_ = std::move(entries);
    totalToDelete_ = entries_.size();
}

std::expected<void, DeleteOperation::Error> DeleteOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info("DeleteOperation: Delete of '{}' canceled.", remotePath_.generic_string());
        state_ = OperationState::Canceled;
    }
    return {};
}