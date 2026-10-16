#include <backend/local_operation_queue.hpp>
#include <backend/local_delete_guard.hpp>
#include <backend/rpc_helper.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <utility/localized_message.hpp>
#include <log/log.hpp>

#include <boost/asio/post.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <utility>

LocalOperationQueue::LocalOperationQueue(
    boost::asio::any_io_executor executor,
    std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    std::shared_ptr<std::atomic_bool const> localDeletionPrevented
)
    : RpcHelper::StrandRpc{executor, std::move(strand), wnd, hub}
    , localDeletionPrevented_{std::move(localDeletionPrevented)}
{}

std::string LocalOperationQueue::rpcName(std::string_view event) const
{
    return fmt::format("LocalOperationQueue::{}", event);
}

void LocalOperationQueue::registerRpc()
{
    on(rpcName("addDelete"))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& operationIdString, std::vector<std::string> const& paths
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("The local operation queue no longer exists");
                const auto added =
                    self->addExplorerDelete(Ids::makeOperationId(operationIdString), paths, std::nullopt, false);
                if (!added)
                    return reply.error(added.error());
                reply(SharedData::success());
            }
        );

    on(rpcName("cancel"))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, Ids::OperationId operationId)
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("The local operation queue no longer exists");
                self->cancel(operationId);
                reply(SharedData::success());
            }
        );
}

std::expected<void, Operation::Error> LocalOperationQueue::addDelete(DeleteRequest request)
{
    if (request.targets.empty())
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::InvalidPath, .extraInfo = "no targets"});
    const auto homes = localHomeDirectories();
    for (auto const& target : request.targets)
    {
        if (auto allowed = refuseProtectedLocalDelete(target, homes); !allowed)
        {
            Log::error("Refusing to delete locally: {}", allowed.error());
            return std::unexpected(
                Operation::Error{.type = Operation::ErrorType::InvalidPath, .extraInfo = target.toUtf8()}
            );
        }
    }

    auto parent = LocalDeleteOperation::commonParentOf(request.targets);
    const auto priority = request.mode == SharedData::OperationMode::PriorityQueued;
    auto operation = std::make_unique<LocalDeleteOperation>(LocalDeleteOperation::LocalDeleteOperationOptions{
        .itemsRemovedProgress =
            [weak = weak_from_this(), operationId = request.operationId](
                std::string const& path, std::uint64_t itemsDeleted, std::uint64_t totalItems
            )
        {
            if (auto self = weak.lock(); self)
            {
                RpcHelper::callRemoteSafely(
                    *self->hub_,
                    self->rpcName("onDeleteProgress"),
                    SharedData::BulkDeleteProgress{
                        .operationId = operationId,
                        .currentFile = path,
                        .filesDeleted = itemsDeleted,
                        .totalFiles = totalItems,
                    }
                );
            }
        },
        .scanProgress =
            [weak = weak_from_this(), operationId = request.operationId](std::uint64_t bytes, std::uint64_t entries)
        {
            if (auto self = weak.lock(); self)
            {
                RpcHelper::callRemoteSafely(
                    *self->hub_,
                    self->rpcName("onScanProgress"),
                    SharedData::ScanProgress{
                        .operationId = operationId,
                        .totalBytes = bytes,
                        .currentIndex = entries,
                        .totalScanned = entries,
                    }
                );
            }
        },
        .targets = std::move(request.targets),
        .recursive = request.recursive,
        .comparedStates = std::move(request.comparedStates),
    });

    const auto position = priority ? std::ranges::find_if(
                                         deletes_,
                                         [](QueuedDelete const& queued)
                                         {
                                             return !queued.priority;
                                         }
                                     )
                                   : deletes_.end();
    deletes_.insert(
        position,
        QueuedDelete{
            .operationId = request.operationId,
            .operation = std::move(operation),
            .owner = std::move(request.owner),
            .held = request.held && !priority,
            .priority = priority,
        }
    );

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = request.operationId,
            .type = SharedData::OperationType::LocalDelete,
            .mode = request.mode,
            .insertRefresh = request.insertRefresh,
            .localPath = std::move(parent),
            .scanPhase = request.recursive,
        }
    );
    schedule();
    return {};
}

std::expected<void, std::string> LocalOperationQueue::addExplorerDelete(
    Ids::OperationId operationId,
    std::vector<std::string> const& pathsUtf8,
    std::optional<Ids::SessionId> owner,
    bool held
)
{
    if (!localDeletionPrevented_ || localDeletionPrevented_->load())
    {
        Log::warn("A local delete was asked for, but deletion is prevented by configuration.");
        return std::unexpected(std::string{"backend.rpcFilesystem.deletionPrevented"});
    }

    std::vector<Paths::LocalPath> targets;
    targets.reserve(pathsUtf8.size());
    const auto homes = localHomeDirectories();
    for (auto const& path : pathsUtf8)
    {
        auto target = localDeleteTarget(path, homes);
        if (!target)
        {
            Log::error("Refusing to delete locally: {}", target.error());
            return std::unexpected(Utility::localizedMessage("backend.rpcFilesystem.removeRefused", path));
        }
        targets.push_back(*std::move(target));
    }

    return addDelete({
                         .operationId = std::move(operationId),
                         .targets = std::move(targets),
                         .recursive = true,
                         .insertRefresh = true,
                         .owner = std::move(owner),
                         .held = held,
                     })
        .transform_error(
            [](Operation::Error const& error)
            {
                return error.toString();
            }
        );
}

void LocalOperationQueue::hold(Ids::SessionId const& owner, bool held)
{
    for (auto& queued : deletes_)
    {
        if (queued.owner == owner && !queued.priority)
            queued.held = held;
    }
    if (!held)
        schedule();
}

void LocalOperationQueue::release(Ids::SessionId const& owner)
{
    for (auto& queued : deletes_)
    {
        if (queued.owner != owner)
            continue;
        queued.owner.reset();
        queued.held = false;
    }
    schedule();
}

void LocalOperationQueue::cancel(Ids::OperationId const& operationId)
{
    const auto found = std::ranges::find(deletes_, operationId, &QueuedDelete::operationId);
    if (found == deletes_.end())
    {
        Log::info("LocalOperationQueue: '{}' is not queued, nothing to cancel.", operationId.value());
        return;
    }
    std::ignore = found->operation->cancel(true);
    complete(*found, SharedData::OperationCompletionReason::Canceled, std::nullopt);
    deletes_.erase(found);
}

bool LocalOperationQueue::work()
{
    const auto runnable = [](QueuedDelete const& queued)
    {
        return !queued.held;
    };
    const auto next = std::ranges::find_if(deletes_, runnable);
    if (next == deletes_.end())
        return false;

    const auto result = next->operation->work();
    if (!result)
    {
        complete(*next, SharedData::OperationCompletionReason::Failed, result.error());
        deletes_.erase(next);
    }
    else if (*result == Operation::WorkStatus::Complete)
    {
        complete(*next, SharedData::OperationCompletionReason::Completed, std::nullopt);
        deletes_.erase(next);
    }
    return std::ranges::any_of(deletes_, runnable);
}

void LocalOperationQueue::schedule()
{
    if (scheduled_)
        return;
    scheduled_ = true;
    boost::asio::post(
        *strand_,
        [weak = weak_from_this()]()
        {
            auto self = weak.lock();
            if (!self)
                return;
            self->scheduled_ = false;
            if (self->work())
                self->schedule();
        }
    );
}

void LocalOperationQueue::complete(
    QueuedDelete const& queued,
    SharedData::OperationCompletionReason reason,
    std::optional<Operation::Error> error
)
{
    if (error)
        Log::error("Local delete failed: {}", error->toString());
    OperationCompleted completed{
        .reason = reason,
        .operationId = queued.operationId,
        .completionTime = std::chrono::system_clock::now(),
        .error = std::move(error),
        .failedEntries = queued.operation->getFailed(),
    };
    if (completionObserver_)
        completionObserver_(completed);
    RpcHelper::callRemoteSafely(*hub_, rpcName("onOperationCompleted"), completed);
}

void LocalOperationQueue::setCompletionObserver(std::function<void(OperationCompleted const&)> observer)
{
    completionObserver_ = std::move(observer);
}
