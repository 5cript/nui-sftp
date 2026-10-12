#include <backend/sftp/delete_operation.hpp>
#include <paths/native.hpp>

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

    if (scanPhase_)
    {
        auto result = stepScanPhase(scanPhase_);
        if (!result.has_value())
            return enterErrorState<WorkStatus>(std::move(result).error());
        return result;
    }

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
                if (remotePath_.empty() || remotePath_.isRoot())
                {
                    Log::error("DeleteOperation: Refusing to delete '{}'.", remotePath_.value());
                    return enterErrorState<WorkStatus>({.type = ErrorType::InvalidPath, .extraInfo = remotePath_.value()});
                }
                targets_ = {remotePath_};
                totalToDelete_ = 1;
                filesRemovedProgress_ = {};
            }
            else if (!scanDelivered_)
            {
                // Without the scanned tree a recursive delete would silently delete nothing.
                Log::error("DeleteOperation: Recursive delete of '{}' has no scan result.", remotePath_.value());
                return enterErrorState<WorkStatus>({.type = ErrorType::ScanIncomplete});
            }
            else if (invalidScanEntry_)
            {
                Log::error(
                    "DeleteOperation: Scan of '{}' returned '{}', which is not a path below it.",
                    remotePath_.value(),
                    *invalidScanEntry_
                );
                return enterErrorState<WorkStatus>({.type = ErrorType::InvalidPath, .extraInfo = *invalidScanEntry_});
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
            // Nothing deleted at all is a failure, some items left behind a partial success.
            if (totalToDelete_ > 0 && failedEntries_.size() == totalToDelete_)
                return enterErrorState<WorkStatus>(failedEntries_.back().second);
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

std::expected<void, DeleteOperation::Error> DeleteOperation::removeOnce(Paths::RemotePath const& path)
{
    auto fut = sftp_->removeAll({path});
    if (fut.wait_for(futureTimeout_) != std::future_status::ready)
        return enterErrorState<void>({.type = ErrorType::FutureTimeout});

    // One item that cannot be deleted does not keep the others, its directories then stay as well. An item that is
    // already gone, after a resume for one, counts as deleted.
    if (const auto result = fut.get(); !result.has_value() && result.error().sftpError != SSH_FX_NO_SUCH_FILE)
    {
        Log::error("DeleteOperation: Failed to delete '{}': {}", path.value(), result.error().toString());
        failedEntries_.emplace_back(
            path.value(), Error{.type = ErrorType::DeleteFailed, .sftpError = result.error(), .extraInfo = path.value()}
        );

    ++deletedThusFar_;
    if (filesRemovedProgress_)
        filesRemovedProgress_(path.value(), deletedThusFar_, totalToDelete_);
    return {};
}

void DeleteOperation::setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t /* totalBytes */)
{
    targets_.clear();
    targets_.reserve(entries.size());
    for (auto const& entry : entries)
    {
        const auto path = SharedData::fullPath(entries, entry);
        auto target = Utility::toRemotePath(path);
        if (!target || target->isRoot() || !target->isWithin(remotePath_))
        {
            invalidScanEntry_ = Utility::pathToUtf8Generic(path);
            targets_.clear();
            break;
        }
        targets_.push_back(*std::move(target));
    }
    totalToDelete_ = targets_.size();
    scanDelivered_ = true;
}

void DeleteOperation::setTargets(std::vector<Paths::RemotePath> targets)
{
    targets_.clear();
    for (auto& target : targets)
    {
        if (target.empty() || target.isRoot())
        {
            invalidScanEntry_ = target.value();
            break;
        }
        targets_.push_back(std::move(target));
    }
    if (invalidScanEntry_)
        targets_.clear();
    totalToDelete_ = targets_.size();
    scanDelivered_ = true;
}

void DeleteOperation::scanFirst(std::unique_ptr<ScanOperation> scan)
{
    auto phase = std::make_unique<ScanPhase<ScanOperation>>(
        [this](ScanOperation& finished)
        {
            setScanResult(finished.ejectEntries(), finished.totalBytes());
        }
    );
    phase->addRoot(std::move(scan));
    scanPhase_ = std::move(phase);
}

std::expected<void, DeleteOperation::Error> DeleteOperation::cancel(bool adoptCancelState)
{
    if (scanPhase_)
        scanPhase_->cancel(adoptCancelState);

    if (adoptCancelState)
    {
        Log::info("DeleteOperation: Delete of '{}' canceled.", remotePath_.value());
        state_ = OperationState::Canceled;
    }
    return {};
}