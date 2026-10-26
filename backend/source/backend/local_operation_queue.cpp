#include <backend/local_operation_queue.hpp>
#include <backend/local_delete_guard.hpp>
#include <backend/rpc_helper.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <utility/localized_message.hpp>
#include <paths/common_parent.hpp>
#include <log/log.hpp>

#include <boost/asio/post.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace
{
    /**
     * @brief The copy progress as the bulk transfer cards show it, the file being copied as the one in flight.
     */
    SharedData::BulkProgress bulkProgressOf(
        Ids::OperationId const& operationId,
        LocalCopyOperation::Progress const& progress,
        std::chrono::steady_clock::duration elapsed
    )
    {
        const auto seconds = std::chrono::duration<double>(elapsed).count();
        auto bulk = SharedData::BulkProgress{
            .operationId = operationId,
            .fileCurrentIndex = progress.entriesDone,
            .fileCount = progress.entriesTotal,
            .bytesCurrent = progress.bytesDone,
            .bytesTotal = progress.bytesTotal,
            .bytesPerSecond =
                seconds > 0.0 ? static_cast<std::int64_t>(static_cast<double>(progress.bytesDone) / seconds) : 0,
            .inFlight = {},
            .failedEntryIndices = {},
        };
        if (progress.currentFile)
        {
            bulk.inFlight.push_back({
                .file = *progress.currentFile,
                .entryIndex = progress.entriesDone,
                .bytes = progress.fileBytesDone,
                .totalBytes = progress.fileBytesTotal,
            });
        }
        return bulk;
    }
}

Operation& LocalOperationQueue::QueuedOperation::base() const
{
    return std::visit(
        [](auto const& operation) -> Operation&
        {
            return *operation;
        },
        operation
    );
}

std::vector<std::pair<std::string, Operation::Error>> const& LocalOperationQueue::QueuedOperation::failed() const
{
    return std::visit(
        [](auto const& operation) -> std::vector<std::pair<std::string, Operation::Error>> const&
        {
            return operation->getFailed();
        },
        operation
    );
}

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

    on(rpcName("addCopy"))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& operationIdString, SharedData::LocalCopyRequest request
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("The local operation queue no longer exists");
                const auto added = self->addCopy({
                    .operationId = Ids::makeOperationId(operationIdString),
                    .request = std::move(request),
                });
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

    enqueue(QueuedOperation{
        .operationId = request.operationId,
        .operation = std::move(operation),
        .owner = std::move(request.owner),
        .held = request.held && !priority,
        .priority = priority,
    });

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
    return {};
}

std::expected<void, std::string> LocalOperationQueue::addCopy(CopyRequest request)
{
    auto& entries = request.request.entries;
    if (entries.empty())
        return std::unexpected(std::string{"backend.localCopy.nothingToCopy"});
    std::vector<Paths::LocalPath> sources;
    sources.reserve(entries.size());
    std::ranges::transform(entries, std::back_inserter(sources), &SharedData::LocalCopyEntry::source);
    const auto move = request.request.move;
    if (move)
    {
        const auto homes = localHomeDirectories();
        for (auto const& source : sources)
        {
            if (auto allowed = refuseProtectedLocalDelete(source, homes); !allowed)
            {
                Log::error("Refusing to move locally: {}", allowed.error());
                return std::unexpected(Utility::localizedMessage("backend.localCopy.moveRefused", source.toUtf8()));
            }
        }
    }

    std::vector<Paths::LocalPath> destinations;
    destinations.reserve(entries.size());
    std::ranges::transform(entries, std::back_inserter(destinations), &SharedData::LocalCopyEntry::destination);
    auto sourceParent = Paths::commonParentOf(std::span<Paths::LocalPath const>{sources});
    auto destinationParent = Paths::commonParentOf(std::span<Paths::LocalPath const>{destinations});

    auto operation = std::make_unique<LocalCopyOperation>(LocalCopyOperation::LocalCopyOperationOptions{
        .progress =
            [weak = weak_from_this(), operationId = request.operationId, start = std::chrono::steady_clock::now()](
                LocalCopyOperation::Progress const& progress
            )
        {
            if (auto self = weak.lock(); self)
            {
                RpcHelper::callRemoteSafely(
                    *self->hub_,
                    self->rpcName("onCopyProgress"),
                    bulkProgressOf(operationId, progress, std::chrono::steady_clock::now() - start)
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
        .entries = std::move(entries),
        .move = move,
    });

    enqueue(QueuedOperation{
        .operationId = request.operationId,
        .operation = std::move(operation),
        .owner = std::move(request.owner),
        .held = request.held,
        .priority = false,
    });

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = request.operationId,
            .type = move ? SharedData::OperationType::LocalMove : SharedData::OperationType::LocalCopy,
            .mode = SharedData::OperationMode::Queued,
            .insertRefresh = true,
            .localPath = std::move(sourceParent),
            .scanPhase = true,
            .localDestination = std::move(destinationParent),
        }
    );
    return {};
}

void LocalOperationQueue::enqueue(QueuedOperation queued)
{
    const auto position = queued.priority ? std::ranges::find_if(
                                                operations_,
                                                [](QueuedOperation const& other)
                                                {
                                                    return !other.priority;
                                                }
                                            )
                                          : operations_.end();
    operations_.insert(position, std::move(queued));
    schedule();
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
    for (auto& queued : operations_)
    {
        if (queued.owner == owner && !queued.priority)
            queued.held = held;
    }
    if (!held)
        schedule();
}

void LocalOperationQueue::release(Ids::SessionId const& owner)
{
    for (auto& queued : operations_)
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
    const auto found = std::ranges::find(operations_, operationId, &QueuedOperation::operationId);
    if (found == operations_.end())
    {
        Log::info("LocalOperationQueue: '{}' is not queued, nothing to cancel.", operationId.value());
        return;
    }
    std::ignore = found->base().cancel(true);
    complete(*found, SharedData::OperationCompletionReason::Canceled, std::nullopt);
    operations_.erase(found);
}

bool LocalOperationQueue::work()
{
    const auto runnable = [](QueuedOperation const& queued)
    {
        return !queued.held;
    };
    const auto next = std::ranges::find_if(operations_, runnable);
    if (next == operations_.end())
        return false;

    const auto result = next->base().work();
    if (!result)
    {
        complete(*next, SharedData::OperationCompletionReason::Failed, result.error());
        operations_.erase(next);
    }
    else if (*result == Operation::WorkStatus::Complete)
    {
        complete(*next, SharedData::OperationCompletionReason::Completed, std::nullopt);
        operations_.erase(next);
    }
    return std::ranges::any_of(operations_, runnable);
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
    QueuedOperation const& queued,
    SharedData::OperationCompletionReason reason,
    std::optional<Operation::Error> error
)
{
    if (error)
        Log::error("Local operation failed: {}", error->toString());
    OperationCompleted completed{
        .reason = reason,
        .operationId = queued.operationId,
        .completionTime = std::chrono::system_clock::now(),
        .error = std::move(error),
        .failedEntries = queued.failed(),
    };
    if (completionObserver_)
        completionObserver_(completed);
    RpcHelper::callRemoteSafely(*hub_, rpcName("onOperationCompleted"), completed);
}

void LocalOperationQueue::setCompletionObserver(std::function<void(OperationCompleted const&)> observer)
{
    completionObserver_ = std::move(observer);
}
