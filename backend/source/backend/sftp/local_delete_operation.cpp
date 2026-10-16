#include <backend/sftp/local_delete_operation.hpp>
#include <utility/link_safe_remove.hpp>

#include <filesystem>
#include <system_error>

LocalDeleteOperation::LocalDeleteOperation(LocalDeleteOperationOptions options)
    : Operation{}
    , options_{std::move(options)}
{}

std::expected<LocalDeleteOperation::WorkStatus, LocalDeleteOperation::Error> LocalDeleteOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case NotStarted:
        case Preparing:
        case Prepared:
        {
            for (auto const& target : options_.targets)
            {
                if (target.empty() || target.isRoot())
                {
                    Log::error("LocalDeleteOperation: Refusing to delete '{}'.", target.toUtf8());
                    return enterErrorState<WorkStatus>({.type = ErrorType::InvalidPath, .extraInfo = target.toUtf8()});
                }
            }
            state_ = Running;
            [[fallthrough]];
        }
        case Running:
        {
            const auto total = options_.targets.size();
            if (deletedThusFar_ < total)
            {
                auto const& target = options_.targets[total - deletedThusFar_ - 1];
                std::error_code error{};
                if (Utility::isLinkOrJunction(target.native(), error) && !error)
                    Utility::removeAllNoFollow(target.native(), error);
                else if (!error)
                    std::filesystem::remove(target.native(), error);
                // One item that cannot be deleted does not keep the others, its directories then stay as well.
                if (error)
                {
                    Log::error("LocalDeleteOperation: Failed to delete '{}': {}", target.toUtf8(), error.message());
                    failedEntries_.emplace_back(
                        target.toUtf8(),
                        Error{
                            .type = ErrorType::DeleteFailed,
                            .extraInfo = fmt::format("{}: {}", target.toUtf8(), error.message())
                        }
                    );
                }
                ++deletedThusFar_;
                options_.itemsRemovedProgress(target.toUtf8(), deletedThusFar_, total);
                if (deletedThusFar_ < total)
                    return WorkStatus::MoreWork;
            }
            // Nothing deleted at all is a failure, some items left behind a partial success.
            if (total > 0 && failedEntries_.size() == total)
                return enterErrorState<WorkStatus>(failedEntries_.back().second);
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case Finalizing:
        {
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case Completed:
        case PartialSuccess:
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        case Failed:
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        case Canceled:
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
    }
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, LocalDeleteOperation::Error> LocalDeleteOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
        state_ = OperationState::Canceled;
    return {};
}
