#include <backend/sftp/local_delete_operation.hpp>
#include <utility/error_message.hpp>
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
            state_ = Running;
            [[fallthrough]];
        }
        case Running:
        {
            const auto total = options_.targets.size();
            if (deletedThusFar_ < total)
            {
                auto const& [path, error] = failedEntries_.front();
                return enterErrorState<WorkStatus>(Error{
                    .type = ErrorType::DeleteFailed,
                    .extraInfo = fmt::format("{}: {}", path, error.extraInfo.value_or(std::string{})),
                });
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
