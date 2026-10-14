#include <backend/sftp/local_delete_operation.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>

std::optional<Paths::LocalPath> LocalDeleteOperation::commonParentOf(std::span<Paths::LocalPath const> targets)
{
    return Paths::commonParentOf(targets);
}

LocalDeleteOperation::LocalDeleteOperation(LocalDeleteOperationOptions options)
    : Operation{}
    , options_{std::move(options)}
    , counting_{options_.recursive}
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
            const auto deadline = std::chrono::steady_clock::now() + options_.stepBudget;
            const bool finished = options_.recursive ? workRecursive(deadline) : workFlat(deadline);
            reportProgress(finished);
            if (!finished)
                return WorkStatus::MoreWork;
            walk_.reset();
            if (failureCount_ > 0 && removedEntries_ == 0 && completedTargets_ == 0)
            {
                auto const& [path, error] = failedEntries_.front();
                return enterErrorState<WorkStatus>(Error{
                    .type = ErrorType::DeleteFailed,
                    .extraInfo = fmt::format("{}: {}", path, error.extraInfo.value_or(std::string{})),
                });
            }
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

bool LocalDeleteOperation::workFlat(std::chrono::steady_clock::time_point deadline)
{
    auto const& targets = options_.targets;
    do
    {
        if (targetIndex_ >= targets.size())
            return true;
        const auto index = targets.size() - targetIndex_ - 1;
        auto const& target = targets[index];
        const auto compared = index < options_.comparedStates.size()
            ? std::optional{options_.comparedStates[index]}
            : std::nullopt;
        if (auto unchanged = ComparedStateCheck::requireUnchangedLocal(target.native(), compared); !unchanged)
        {
            recordFailure(target.toUtf8(), std::move(unchanged).error());
        }
        else
        {
            std::error_code error{};
            if (Utility::removeNoFollow(target.native(), error))
                ++removedEntries_;
            if (error)
                recordFailure(target.native(), error);
            else
                ++completedTargets_;
        }
        lastItem_ = target.toUtf8();
        ++targetIndex_;
    } while (std::chrono::steady_clock::now() < deadline);
    return targetIndex_ >= targets.size();
}

bool LocalDeleteOperation::workRecursive(std::chrono::steady_clock::time_point deadline)
{
    auto const& targets = options_.targets;
    while (true)
    {
        if (!walk_)
        {
            if (targetIndex_ >= targets.size())
            {
                if (!counting_)
                    return true;
                counting_ = false;
                targetIndex_ = 0;
                options_.scanProgress(countedBytes_, countedEntries_);
                continue;
            }
            walk_.emplace(
                targets[targets.size() - targetIndex_ - 1].native(),
                counting_ ? Utility::NoFollowTreeWalk::Mode::Count : Utility::NoFollowTreeWalk::Mode::Remove
            );
        }
        const bool walked = walk_->step(deadline);
        if (!walked)
            return false;
        finishWalk();
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
    }
}

void LocalDeleteOperation::finishWalk()
{
    if (counting_)
    {
        countedEntries_ += walk_->entries();
        countedBytes_ += walk_->bytes();
    }
    else
    {
        removedEntries_ += walk_->entries();
        lastItem_ = Paths::nativeToUtf8(walk_->lastPath());
        for (auto const& failure : walk_->failures())
            recordFailure(failure.path, failure.error);
        if (walk_->failures().empty())
            ++completedTargets_;
    }
    walk_.reset();
    ++targetIndex_;
}

void LocalDeleteOperation::recordFailure(std::filesystem::path const& path, std::error_code const& error)
{
    recordFailure(Paths::nativeToUtf8(path), Error{.type = ErrorType::DeleteFailed, .extraInfo = Utility::errorMessage(error)});
}

void LocalDeleteOperation::recordFailure(std::string const& path, Error error)
{
    ++failureCount_;
    if (failedEntries_.size() >= maximumListedFailures)
        return;
    Log::error("LocalDeleteOperation: Failed to delete '{}': {}", path, error.toString());
    failedEntries_.emplace_back(path, std::move(error));
    if (failedEntries_.size() == maximumListedFailures)
        Log::error("LocalDeleteOperation: Further failures are counted, not listed.");
}

std::uint64_t LocalDeleteOperation::handledEntries() const
{
    if (!options_.recursive)
        return targetIndex_;
    const auto running = walk_ && !counting_ ? walk_->entries() + walk_->failures().size() : 0;
    return removedEntries_ + failureCount_ + running;
}

void LocalDeleteOperation::reportProgress(bool final)
{
    const auto now = std::chrono::steady_clock::now();
    if (!final && now - lastProgress_ < options_.progressInterval)
        return;
    lastProgress_ = now;
    if (counting_)
    {
        const auto entries = countedEntries_ + (walk_ ? walk_->entries() : 0);
        const auto bytes = countedBytes_ + (walk_ ? walk_->bytes() : 0);
        return options_.scanProgress(bytes, entries);
    }
    const auto current = handledEntries();
    const auto total = options_.recursive ? std::max<std::uint64_t>(countedEntries_, current) : options_.targets.size();
    if (walk_)
        lastItem_ = Paths::nativeToUtf8(walk_->lastPath());
    options_.itemsRemovedProgress(lastItem_, current, total);
}

std::expected<void, LocalDeleteOperation::Error> LocalDeleteOperation::cancel(bool adoptCancelState)
{
    walk_.reset();
    if (adoptCancelState)
        state_ = OperationState::Canceled;
    return {};
}
