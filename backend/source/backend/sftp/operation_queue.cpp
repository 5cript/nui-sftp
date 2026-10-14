#include <backend/sftp/operation_queue.hpp>
#include <utility/error_message.hpp>
#include <utility/localized_message.hpp>
#include <backend/rpc_helper.hpp>
#include <paths/common_parent.hpp>
#include <paths/native.hpp>
#include <backend/local_delete_guard.hpp>
#include <backend/sftp/parallel_budget.hpp>
#include <backend/sftp/sequential_scan_progress.hpp>
#include <shared_data/file_operations/transfer_progress.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operations_reordered.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/is_paused.hpp>

#include <boost/asio/post.hpp>

#include <log/log.hpp>
#include <utility/overloaded.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    /**
     * @brief No path instead of an unset one, which the frontend would refuse to read.
     */
    template <typename PathT>
    std::optional<PathT> unlessEmpty(PathT path)
    {
        if (path.empty())
            return std::nullopt;
        return path;
    }

    /**
     * @brief The closest directory holding the path @p member of every file of a prescanned bulk.
     */
    template <typename FileT, typename PathT>
    PathT commonParentOf(std::vector<FileT> const& files, PathT FileT::* member)
    {
        std::vector<PathT> paths;
        paths.reserve(files.size());
        std::ranges::transform(files, std::back_inserter(paths), member);
        auto parent = Paths::commonParentOf(std::span<PathT const>{paths});
        return parent ? *std::move(parent) : PathT{};
    }

    OperationQueue::OperationCompleted makeCompletedOperation(
        OperationQueue::CompletionReason reason,
        Ids::OperationId operationId,
        Operation const& operation,
        std::optional<Operation::Error> error = std::nullopt
    )
    {
        return operation.visit(
            Utility::overloaded(
                [reason, operationId, error](DownloadOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .localPath = unlessEmpty(op.localPath()),
                        .remotePath = unlessEmpty(op.remotePath()),
                        .error = error,
                    };
                },
                [reason, operationId, error](ScanOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .remotePath = unlessEmpty(op.remotePath()),
                        .error = error,
                    };
                },
                [reason, operationId, error](LocalScanOperation const& op)
                {
                    completed.remotePath = unlessEmpty(scan.remotePath());
                },
                [reason, operationId, error](UploadOperation const& op)
                {
                    completed.localPath = unlessEmpty(scan.localPath());
                },
                [reason, operationId, error](DeleteOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .remotePath = unlessEmpty(op.remotePath()),
                        .error = error,
                        .failedEntries = op.getFailed(),
                    };
                },
                [reason, operationId, error](LocalDeleteOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .error = error,
                        .failedEntries = op.getFailed(),
                    };
                },
                [reason, operationId, error](RenameOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .remotePath = unlessEmpty(op.destinationPath()),
                        .error = error,
                    };
                },
                [reason, operationId, error](ArchiveDownloadOperation const&)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .error = error,
                    };
                },
                [reason, operationId, error](ArchiveUploadOperation const&)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .error = error,
                    };
                },
                [reason, operationId](std::nullopt_t)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                    };
                }
            )
        );
    }

    DownloadOperation::DownloadOperationOptions
    resolveDownloadOptions(Persistence::DownloadOptions const& opts, std::chrono::seconds futureTimeout)
    {
        const auto def = DownloadOperation::DownloadOperationOptions{};
        return {
            .tempFileSuffix = opts.tempFileSuffix.value_or(def.tempFileSuffix),
            .mayOverwrite = opts.mayOverwrite.value_or(def.mayOverwrite),
            .reserveSpace = opts.reserveSpace.value_or(def.reserveSpace),
            .tryContinue = opts.tryContinue.value_or(def.tryContinue),
            .inheritPermissions = opts.inheritPermissions.value_or(def.inheritPermissions),
            .doCleanup = opts.doCleanup.value_or(def.doCleanup),
            .filePermissions = opts.customFilePermissions ? opts.customFilePermissions : def.filePermissions,
            .directoryPermissions =
                opts.customDirectoryPermissions ? opts.customDirectoryPermissions : def.directoryPermissions,
            .futureTimeout = futureTimeout,
            .symlinkHandling = opts.symlinkHandling.value_or(def.symlinkHandling),
            .markAsDownloaded = opts.markAsDownloaded.value_or(def.markAsDownloaded),
        };
    }

    UploadOperation::UploadOperationOptions
    resolveUploadOptions(Persistence::UploadOptions const& opts, std::chrono::seconds futureTimeout)
    {
        const auto def = UploadOperation::UploadOperationOptions{};
        return {
            .tempFileSuffix = opts.tempFileSuffix.value_or(def.tempFileSuffix),
            .mayOverwrite = opts.mayOverwrite.value_or(def.mayOverwrite),
            .tryContinue = opts.tryContinue.value_or(def.tryContinue),
            .inheritPermissions = opts.inheritPermissions.value_or(def.inheritPermissions),
            .filePermissions = opts.customFilePermissions ? opts.customFilePermissions : def.filePermissions,
            .directoryPermissions =
                opts.customDirectoryPermissions ? opts.customDirectoryPermissions : def.directoryPermissions,
            .futureTimeout = futureTimeout,
            .symlinkHandling = opts.symlinkHandling.value_or(def.symlinkHandling),
        };
    }
}

OperationQueue::OperationQueue(
    boost::asio::any_io_executor executor,
    std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    Persistence::SftpOptions sftpOpts,
    Ids::SessionId sessionId,
    int parallelism
)
    : RpcHelper::StrandRpc{executor, strand, wnd, hub}
    , sftpOpts_{std::move(sftpOpts)}
    , sessionId_{std::move(sessionId)}
    , parallelism_{std::max(1, parallelism)}
    , bulkConcurrency_{std::clamp(
          sftpOpts_.bulkConcurrency.value_or(Persistence::defaultBulkConcurrency),
          1,
          std::max(1, parallelism_ - 1)
      )}
{}

std::string OperationQueue::rpcName(std::string_view event) const
{
    return fmt::format("OperationQueue::{}::{}", sessionId_.value(), event);
}

auto OperationQueue::makeScanProgressCallback(std::string_view eventName, Ids::OperationId operationId)
    -> std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>
{
    return [weak = weak_from_this(),
               name = rpcName(eventName),
               operationId](std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)
    {
        auto self = weak.lock();
        if (!self)
            return;
        // May be invoked from the SFTP processing strand (batched umbrella) — marshal the
        // RPC call back onto the asio strand that owns the hub.
        self->within_strand_do(
            [weak, name, operationId, totalBytes, currentIndex, totalScanned]()
            {
                auto self = weak.lock();
                if (!self)
                    return;
                RpcHelper::callRemoteSafely(
                    *self->hub_,
                    name,
                    SharedData::ScanProgress{
                        .operationId = operationId,
                        .totalBytes = totalBytes,
                        .currentIndex = currentIndex,
                        .totalScanned = totalScanned,
                    }
                );
            }
        );
    };
}

auto OperationQueue::makeDeleteProgressCallback(Ids::OperationId operationId)
    -> std::function<void(std::string const&, std::uint64_t, std::uint64_t)>
{
    return [weak = weak_from_this(), operationId, name = rpcName("onDeleteProgress")](
               std::string const& path, std::uint64_t itemsDeleted, std::uint64_t totalItems
           )
    {
        auto self = weak.lock();
        if (!self)
            return;
        RpcHelper::callRemoteSafely(
            *self->hub_,
            name,
            SharedData::BulkDeleteProgress{
                .operationId = operationId,
                .currentFile = path,
                .filesDeleted = itemsDeleted,
                .totalFiles = totalItems,
            }
        );
    };
}

auto OperationQueue::makeBulkProgressCallback(std::string_view eventName, Ids::OperationId operationId)
    -> BulkProgressCallback
{
    return [weak = weak_from_this(), name = rpcName(eventName), operationId](SharedData::BulkProgress progress)
    {
        auto self = weak.lock();
        if (!self)
            return;
        // Invoked from the SFTP processing strand, marshal the RPC call back onto the asio
        // strand that owns the hub.
        progress.operationId = operationId;
        self->within_strand_do(
            [weak, name, progress = std::move(progress)]()
            {
                auto self = weak.lock();
                if (!self)
                    return;
                RpcHelper::callRemoteSafely(*self->hub_, name, progress);
            }
        );
    };
}

void OperationQueue::cancelAll()
{
    within_strand_do(
        [weak = weak_from_this()]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            self->operations_.clear();
            self->priorityOperations_.clear();
            self->syncRecordings_.clear();
            auto waiting = std::exchange(self->syncScanCallbacks_, {});
            for (auto& [id, callback] : waiting)
                callback(std::unexpected(Utility::localizedMessage("backend.sync.scanCancelled")));
            Log::info("All operations in the queue have been canceled.");
        }
    );
}

void OperationQueue::cancel(Ids::OperationId id)
{
    within_strand_do(
        [weak = weak_from_this(), id = std::move(id)]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            auto cancelPredicate = [id](auto& op)
            {
                bool isMatch = op.first == id;
                if (isMatch)
                    std::ignore = op.second->cancel(true);
                return isMatch;
            };

            const auto erasedFromRegular = std::erase_if(self->operations_, cancelPredicate);
            if (erasedFromRegular == 0)
                std::erase_if(self->priorityOperations_, cancelPredicate);
            self->failSyncScan(id, Utility::localizedMessage("backend.sync.scanCancelled"));
        }
    );
}

void OperationQueue::moveOperation(Ids::OperationId operationId, std::size_t newIndex)
{
    within_strand_do(
        [weak = weak_from_this(), operationId = std::move(operationId), newIndex]() mutable
        {
            auto self = weak.lock();
            if (!self)
                return;

            // Pessimistic resolution: every exit path emits onOperationsReordered
            // so the frontend always gets a definitive "this move was/was not
            // applied" — never silently dropped. The frontend's listener relies
            // on this to apply or discard the user's intended reorder.
            auto emitResolution = [&self, &operationId](std::size_t resolvedIndex, bool applied)
            {
                RpcHelper::callRemoteSafely(
                    *self->hub_,
                    self->rpcName("onOperationsReordered"),
                    SharedData::OperationsReordered{
                        .operationId = operationId,
                        .newIndex = static_cast<std::int32_t>(resolvedIndex),
                        .applied = applied,
                    }
                );
            };

            // Pause is the only safe window: while running, work() can be
            // mid-batch with raw pointers into the deque (see workQueue()),
            // and reordering would alter the indices it iterates.
            if (!self->paused_)
            {
                Log::warn("OperationQueue::moveOperation refused — queue not paused (op '{}')", operationId.value());
                return emitResolution(newIndex, false);
            }

            // Priority queue is intentionally not user-reorderable.
            for (auto const& [pid, _op] : self->priorityOperations_)
            {
                if (pid == operationId)
                {
                    Log::warn("OperationQueue::moveOperation refused — '{}' is a priority op", operationId.value());
                    return emitResolution(newIndex, false);
                }
            }

            auto& queue = self->operations_;
            std::size_t oldIndex = 0;
            bool found = false;
            for (std::size_t idx = 0; idx < queue.size(); ++idx)
            {
                if (queue[idx].first == operationId)
                {
                    oldIndex = idx;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                // Op completed/canceled between click and strand tick — silent
                // resolution; the frontend already removed the card via
                // onOperationCompleted, so applied=false is purely informational.
                return emitResolution(newIndex, false);
            }

            if (queue.empty())
                return emitResolution(newIndex, false);
            if (newIndex >= queue.size())
                newIndex = queue.size() - 1;
            if (oldIndex == newIndex)
                return emitResolution(newIndex, false);

            auto entry = std::move(queue[oldIndex]);
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(oldIndex));
            queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(entry));

            Log::info(
                "OperationQueue::moveOperation '{}' from {} to {} (queue size {})",
                operationId.value(),
                oldIndex,
                newIndex,
                queue.size()
            );

            emitResolution(newIndex, true);
        }
    );
}

void OperationQueue::completeOperation(SharedData::OperationCompleted&& operationCompleted)
{
    within_strand_do(
        [weak = weak_from_this(), operationCompleted = std::move(operationCompleted)]() mutable
        {
            auto self = weak.lock();
            if (!self)
                return;

            if (operationCompleted.error)
                Log::error("Operation failed: {}", operationCompleted.error->toString());

            // Drop any resume backup tied to this operation — it has run to
            // completion (success, failure, or cancel) and no longer needs
            // a backup for reconnect adoption.
            self->bulkResumeStash_.erase(operationCompleted.operationId);

            self->recordSyncCompletion(operationCompleted);
            if (self->completionObserver_)
                self->completionObserver_(operationCompleted);
            RpcHelper::callRemoteSafely(*self->hub_, self->rpcName("onOperationCompleted"), operationCompleted);
        }
    );
}

void OperationQueue::setCompletionObserver(std::function<void(OperationCompleted const&)> observer)
{
    completionObserver_ = std::move(observer);
}

void OperationQueue::dumpBulkResumes(BulkResumeRegistry& registry)
{
    if (bulkResumeStash_.empty())
        return;
    Log::info("OperationQueue::dumpBulkResumes: persisting {} bulk-operation backup(s)", bulkResumeStash_.size());
    for (auto& [opId, entry] : bulkResumeStash_)
        registry.store(opId, std::move(entry));
    bulkResumeStash_.clear();
}

void OperationQueue::adoptBulkResume(
    SecureShell::SftpSession& sftp,
    Ids::OperationId const& operationId,
    BulkResumeEntry entry
)
{
    Log::info(
        "OperationQueue::adoptBulkResume: re-issuing bulk operation '{}' ({} entries)",
        operationId.value(),
        std::visit(
            [](auto const& request)
            {
                return request.entries.size();
            },
            entry.request
        )
    );

    // A transfer backup holds either the files of one bulk card or a single directory, both resume under the
    // stashed id so the surviving card re-attaches.
    auto operationIdFor = [&operationId](std::size_t)
    {
        return operationId;
    };

    std::visit(
        Utility::overloaded{
            [&](SharedData::BulkDownloadRequest const& request)
            {
                addBulkDownloadOperation(sftp, request, operationIdFor, operationId);
            },
            [&](SharedData::BulkUploadRequest const& request)
            {
                addBulkUploadOperation(sftp, request, operationIdFor, operationId);
            },
            [&](SharedData::BulkDeleteRequest const& request)
            {
                addBulkDeleteOperation(sftp, request, operationId);
            },
        },
        entry.request
    );
}

void OperationQueue::deepPause(bool pause)
{
    // Every operation, not just the slot window: a reorder can push an operation with
    // running transfers out of the window, and it must still stop and start with the queue.
    for (auto& [id, operation] : operations_)
        operation->pause(pause);
}

bool OperationQueue::workQueue(std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>>& queue)
{
    // Assumed in (asio) strand.

    // Distribute the transfer-slot budget over the head of the queue; barrier ops stop the run.
    const auto eligible = collectEligibleOperations(queue, parallelism_);
    if (eligible.empty())
        return false;

    // Any strand-using op lets us share a single SFTP processing-thread umbrella across
    // every eligible op's workInStrand() call. Ops that do not use the strand run on the
    // caller afterwards: their work() blocks on that very thread and would deadlock inside it.
    SecureShell::ProcessingStrand* strand = nullptr;
    for (const auto& item : eligible)
    {
        auto* operation = queue[item.index].second.get();
        operation->setParallelBudget(item.slots);
        if (operation->usesStrand() && !strand)
            strand = operation->strand();
    }

    struct ItemResult
    {
        std::size_t queueIndex;
        std::expected<Operation::WorkStatus, Operation::Error> result;
    };

    const auto isTerminal = [](ItemResult const& item)
    {
        return !item.result.has_value() || item.result.value() == Operation::WorkStatus::Complete;
    };

    auto runBatch = [&queue, &eligible, &isTerminal](std::optional<bool> onlyUsesStrand)
    {
        std::vector<ItemResult> out;
        out.reserve(eligible.size());
        for (const auto& item : eligible)
        {
            const auto index = item.index;
            auto* operation = queue[index].second.get();
            if (onlyUsesStrand.has_value() && operation->usesStrand() != *onlyUsesStrand)
                continue;
            auto res = operation->usesStrand() ? operation->workInStrand() : operation->work();
            out.push_back({index, std::move(res)});
            // Match the single-completion-per-workQueue-cycle semantics of the original
            // sequential driver: stop as soon as anybody completes or fails so subsequent
            // ops see the resulting queue mutation on the next cycle.
            if (isTerminal(out.back()))
                break;
        }
        return out;
    };

    std::vector<ItemResult> results;
    if (strand)
    {
        auto fut = strand->pushPromiseTask(
            [&runBatch]()
            {
                return runBatch(true);
            }
        );
        results = fut.get();
        if (std::ranges::none_of(results, isTerminal))
        {
            auto local = runBatch(false);
            results.insert(results.end(), std::make_move_iterator(local.begin()), std::make_move_iterator(local.end()));
        }
    }
    else
    {
        results = runBatch(std::nullopt);
    }

    bool moreWork = false;
    for (auto& item : results)
    {
        auto& [id, operation] = queue[item.queueIndex];

        if (!item.result.has_value())
        {
            failSyncScan(
                id,
                Utility::localizedMessage(
                    operation->type() == SharedData::OperationType::LocalScan ? "backend.sync.localScanFailed"
                                                                              : "backend.sync.remoteScanFailed",
                    item.result.error().toString()
                )
            );
            completeOperation(
                makeCompletedOperation(OperationQueue::CompletionReason::Failed, id, *operation, item.result.error())
            );
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(item.queueIndex));
            return true;
        }

        const auto workStatus = item.result.value();
        if (workStatus == Operation::WorkStatus::Complete)
        {
            deliverSyncScan(id, *operation);
            completeOperation(makeCompletedOperation(OperationQueue::CompletionReason::Completed, id, *operation));
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(item.queueIndex));
            return true;
        }
        else if (workStatus == Operation::WorkStatus::MoreWork)
        {
            moreWork = true;
            continue;
        }
    }
    return moreWork;
}

void OperationQueue::cancelSiblingScan(Ids::OperationId const& id)
{
    // Assumed in strand. The sync waits for both scans, so the other one is of no use once one failed.
    syncScanCallbacks_.erase(id.value());
    // Posted: the failed scan is still being taken out of the queue the sibling lies in.
    boost::asio::post(
        *strand_,
        [weak = weak_from_this(), id]()
        {
            auto self = weak.lock();
            if (!self)
                return;
            const auto matches = [&id](auto& operation)
            {
                if (operation.first != id)
                    return false;
                std::ignore = operation.second->cancel(true);
                return true;
            };
            if (std::erase_if(self->priorityOperations_, matches) + std::erase_if(self->operations_, matches) > 0)
            {
                self->completeOperation(
                    SharedData::OperationCompleted{
                        .reason = SharedData::OperationCompletionReason::Canceled,
                        .operationId = id,
                        .completionTime = std::chrono::system_clock::now(),
                    }
                );
            }
        }
    );
}

void OperationQueue::failSyncScan(Ids::OperationId const& id, std::string const& reason)
{
    auto callback = syncScanCallbacks_.extract(id.value());
    if (!callback.empty())
        callback.mapped()(std::unexpected(reason));
}

void OperationQueue::deliverSyncScan(Ids::OperationId const& id, Operation& operation)
{
    const auto type = operation.type();
    if (type != SharedData::OperationType::Scan && type != SharedData::OperationType::LocalScan)
        return;

    // Scans of transfers and deletes run inside their consumer, a standalone scan belongs to a sync.
    auto callback = syncScanCallbacks_.extract(id.value());
    if (callback.empty())
    {
        Log::error("OperationQueue: Scan '{}' completed without a sync session waiting for it.", id.value());
        return;
    }
    auto tree = type == SharedData::OperationType::Scan
        ? static_cast<ScanOperation&>(operation).ejectScanTree()
        : static_cast<LocalScanOperation&>(operation).ejectScanTree();
    callback.mapped()(std::move(tree));
}

bool OperationQueue::work()
{
    // Assumed in strand

    // Priority queue always runs, regardless of paused state.
    // Regular queue is effectively paused while priority items are being processed.
    if (!priorityOperations_.empty())
        return workQueue(priorityOperations_);

    if (paused_)
        return false;

    return workQueue(operations_);
}

bool OperationQueue::paused() const
{
    return paused_;
}

bool OperationQueue::hasPriorityWork() const
{
    return !priorityOperations_.empty();
}

void OperationQueue::paused(bool pause)
{
    within_strand_do(
        [weak = weak_from_this(), pause]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            self->paused_ = pause;
            self->deepPause(pause);
        }
    );
}

std::expected<void, Operation::Error> OperationQueue::addDownloadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    Paths::LocalPath const& localPath,
    Paths::RemotePath const& remotePath,
    bool allowOverwrite,
    bool isBigFile,
    bool insertRefresh,
    bool createMissingDirectories,
    SharedData::OperationMode mode
)
{
    // Assumed in strand

    auto fut = sftp.lstat(remotePath);
    if (fut.wait_for(sftpOpts_.operationTimeout.value_or(defaultFutureTimeout)) != std::future_status::ready)
    {
        Log::error("Failed to stat remote sftp file: timeout");
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FutureTimeout});
    }

    const auto result = fut.get();
    if (!result.has_value())
    {
        Log::error("Failed to stat remote sftp file: {}", result.error().message);
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::SftpError, .sftpError = result.error()});
    }

    auto transferOptions = sftpOpts_.downloadOptions.value_or(Persistence::DownloadOptions{});
    if (allowOverwrite)
        transferOptions.mayOverwrite = allowOverwrite;

    const auto resolvedTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);

    if (result->isRegularFile() || result->isSymlink())
    {
        auto opts = resolveDownloadOptions(transferOptions, resolvedTimeout);
        opts.remotePath = remotePath;
        opts.localPath = localPath;
        opts.bigFileOptimized = isBigFile;
        opts.entry = result.value();
        opts.createMissingDirectories = createMissingDirectories;
        opts.progressCallback = [weak = weak_from_this(), operationId, name = rpcName("onDownloadProgress")](
                                    auto min, auto max, auto current, auto bytesPerSecond
                                )
        {
            auto self = weak.lock();
            if (!self)
                return;
            // Invoked from the SFTP processing strand via workInStrand — marshal back.
            self->within_strand_do(
                [weak, name, operationId, min, max, current, bytesPerSecond]()
                {
                    auto self = weak.lock();
                    if (!self)
                        return;
                    RpcHelper::callRemoteSafely(
                        *self->hub_,
                        name,
                        SharedData::TransferProgress{
                            .operationId = operationId,
                            .min = min,
                            .max = max,
                            .current = current,
                            .bytesPerSecond = bytesPerSecond,
                        }
                    );
                }
            );
        };

        auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
        targetQueue.emplace_back(operationId, std::make_unique<DownloadOperation>(sftp, std::move(opts)));

        Log::info("Calling OperationQueue::{}::onOperationAdded", sessionId_.value());
        RpcHelper::callRemoteSafely(
            *hub_,
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::Download,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .totalBytes = result->size,
                .localPath = localPath,
                .remotePath = remotePath,
                .allowOverwrite = allowOverwrite,
            }
        );

        return {};
    }
    else if (result->isDirectory())
    {
        auto bulk = std::make_unique<BulkDownloadOperation>(
            sftp,
            BulkDownloadOperation::BulkDownloadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkDownloadProgress", operationId),
                .remotePath = remotePath,
                .localPath = localPath,
                .individualOptions = resolveDownloadOptions(transferOptions, resolvedTimeout),
                .failFast = transferOptions.failFast.value_or(false),
                .concurrency = bulkConcurrency_,
            }
        );

        // The scan is part of the bulk, so cancelling or moving the card acts on both.
        bulk->scanFirst(
            std::make_unique<ScanOperation>(
                sftp,
                ScanOperation::ScanOperationOptions{
                    .progressCallback = makeScanProgressCallback("onScanProgress", operationId),
                    .remotePath = remotePath,
                    .futureTimeout = resolvedTimeout,
                }
            )
        );

        auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
        targetQueue.emplace_back(operationId, std::move(bulk));

        RpcHelper::callRemoteSafely(
            *hub_,
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::BulkDownload,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .localPath = localPath,
                .remotePath = remotePath,
                .scanPhase = true,
            }
        );

        // Synthetic single-entry resume backup: re-issuing this through
        // adopt walks the same scan+bulk path, so the entry list itself
        // doesn't matter — we just need the directory paths preserved.
        SharedData::BulkDownloadRequest synthetic{
            .entries = {SharedData::BulkDownloadEntry{
                .source = remotePath, .destination = localPath, .isDirectory = true
            }},
            .allowOverwrite = allowOverwrite,
            .insertRefresh = insertRefresh,
            .mode = mode,
        };
        bulkResumeStash_.insert_or_assign(operationId, BulkResumeEntry{.request = std::move(synthetic)});

        return {};
    }
    else
    {
        Log::error("Remote path is neither a file nor a directory: {}.", static_cast<std::uint8_t>(result->type));
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::OperationNotPossibleOnFileType});
    }
}

namespace
{
    /**
     * @brief Map a user-facing 1..9 level to codec-native CompressionOptions.
     *
     * Keeps the frontend ignorant of codec specifics: the slider just says
     * "1 = fastest, 9 = smallest" and this picks a sensible setting per codec.
     */
    TarArchive::CompressionOptions compressionOptionsFromLevel(TarArchive::Compression codec, int userLevel)
    {
        const int clampedLevel = std::clamp(userLevel, 1, 9);
        TarArchive::CompressionOptions options{};
        switch (codec)
        {
            case TarArchive::Compression::Gzip:
                options.gzipLevel = clampedLevel;
                break;
            case TarArchive::Compression::Bzip2:
                options.bzip2BlockSize = clampedLevel;
                break;
            case TarArchive::Compression::Zstd:
                // Linear map 1..9 → 1..22 (rounded).
                options.zstdLevel = 1 + static_cast<int>((clampedLevel - 1) * 21.0 / 8.0 + 0.5);
                break;
            case TarArchive::Compression::Xz:
                options.xzPreset = std::clamp(clampedLevel - 1, 0, 9);
                break;
            case TarArchive::Compression::None:
            case TarArchive::Compression::Auto:
                break;
        }
        return options;
    }
}

std::expected<void, Operation::Error> OperationQueue::addArchiveDownloadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::vector<SharedData::ArchiveDownloadRoot> roots,
    Paths::LocalPath const& localArchivePath,
    TarArchive::Compression compression,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode
)
{
    // Assumed in strand.

    if (roots.empty())
    {
        Log::error("addArchiveDownloadOperation: no roots provided.");
        return std::unexpected(
            Operation::Error{
                .type = Operation::ErrorType::InvalidPath,
                .extraInfo = "archive has no entries",
            }
        );
    }

    std::uint64_t totalPayloadBytes = 0u;
    for (auto const& root : roots)
    {
        // Every root is packed under its own name, the remote root directory has none.
        if (!root.path.filename())
        {
            return std::unexpected(
                Operation::Error{
                    .type = Operation::ErrorType::InvalidPath,
                    .extraInfo = fmt::format("'{}' cannot be archived under its own name", root.path.value()),
                }
            );
        }
        if (root.entry.type == SharedData::FileType::Regular)
            totalPayloadBytes += root.entry.size;
    }
    const auto rootCount = roots.size();

    // Collect each directory root's full remote path so we can enqueue a
    // preceding ScanOperation per root. The archive operation consumes those
    // scans' results via setScanResultForRoot and never recurses itself, so
    // scan work is always visible as a separate queue card.
    std::vector<Paths::RemotePath> directoryRootPaths;
    for (auto const& root : roots)
    {
        if (root.entry.type == SharedData::FileType::Directory)
            directoryRootPaths.push_back(root.path);
    }

    ArchiveDownloadOperation::Options opts{};
    opts.roots = std::move(roots);
    opts.localArchivePath = localArchivePath;
    opts.tempFileSuffix = Persistence::effectiveTempFileSuffix(
        sftpOpts_.downloadOptions.value_or(Persistence::DownloadOptions{}).tempFileSuffix
    );
    opts.compression = compression;
    opts.compressionOptions = compressionOptionsFromLevel(compression, compressionLevel);
    opts.mayOverwrite = mayOverwrite;
    opts.markAsDownloaded = sftpOpts_.downloadOptions.value_or(Persistence::DownloadOptions{}).markAsDownloaded.value_or(true);
    opts.createMissingDirectories = true;
    opts.futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);
    opts.progressCallback = [weak = weak_from_this(), operationId, name = rpcName("onArchiveDownloadProgress")](
                                auto min, auto max, auto current, auto bytesPerSecond
                            )
    {
        auto self = weak.lock();
        if (!self)
            return;
        self->within_strand_do(
            [weak, name, operationId, min, max, current, bytesPerSecond]()
            {
                auto self = weak.lock();
                if (!self)
                    return;
                RpcHelper::callRemoteSafely(
                    *self->hub_,
                    name,
                    SharedData::TransferProgress{
                        .operationId = operationId,
                        .min = min,
                        .max = max,
                        .current = current,
                        .bytesPerSecond = bytesPerSecond,
                    }
                );
            }
        );
    };

    auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;

    // Directory roots are scanned by the archive operation itself before it packs.
    auto archive = std::make_unique<ArchiveDownloadOperation>(sftp, std::move(opts));
    for (auto const& rootPath : directoryRootPaths)
    {
        archive->addScanRoot(
            std::make_unique<ScanOperation>(
                sftp,
                ScanOperation::ScanOperationOptions{
                    .progressCallback = makeScanProgressCallback("onScanProgress", operationId),
                    .remotePath = rootPath,
                    .futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout),
                }
            )
        );
    }
    const auto scanPhase = archive->scanning();
    targetQueue.emplace_back(operationId, std::move(archive));

    Log::info(
        "OperationQueue::{}::onOperationAdded (ArchiveDownload, {} entries, {} bytes, {} pre-scans)",
        sessionId_.value(),
        rootCount,
        totalPayloadBytes,
        directoryRootPaths.size()
    );
    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::ArchiveDownload,
            .mode = mode,
            .insertRefresh = true,
            .totalBytes = totalPayloadBytes,
            .localPath = localArchivePath,
            .scanPhase = scanPhase,
        }
    );

    return {};
}

std::expected<void, Operation::Error> OperationQueue::addArchiveUploadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::vector<Paths::LocalPath> localPaths,
    Paths::RemotePath const& remoteArchivePath,
    TarArchive::Compression compression,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode
)
{
    // Assumed in strand.
    if (localPaths.empty())
    {
        Log::error("addArchiveUploadOperation: no local paths provided.");
        return std::unexpected(
            Operation::Error{
                .type = Operation::ErrorType::InvalidPath,
                .extraInfo = "archive has no entries",
            }
        );
    }

    // Rough totalBytes estimate for the OperationAdded event; actual progress
    // comes from the per-chunk callback. The resolved-entries lstat happens
    // inside the operation itself.
    std::uint64_t estimatedTotalBytes = 0u;
    for (auto const& localPath : localPaths)
    {
        std::error_code sizeErr{};
        const auto size = std::filesystem::file_size(localPath.native(), sizeErr);
        if (!sizeErr)
            estimatedTotalBytes += size;
    }

    // Local directory roots get a visible LocalScanOperation each; the
    // archive op itself never recurses and instead consumes each scan's
    // output via setScanResultForRoot — keyed by the root's local path.
    std::vector<Paths::LocalPath> directoryRootPaths;
    for (auto const& localPath : localPaths)
    {
        std::error_code isDirErr{};
        if (std::filesystem::is_directory(localPath.native(), isDirErr))
            directoryRootPaths.push_back(localPath);
    }
    const auto rootCount = localPaths.size();

    ArchiveUploadOperation::Options opts{};
    opts.localPaths = std::move(localPaths);
    opts.remoteArchivePath = remoteArchivePath;
    opts.tempFileSuffix = Persistence::effectiveTempFileSuffix(
        sftpOpts_.uploadOptions.value_or(Persistence::UploadOptions{}).tempFileSuffix
    );
    opts.compression = compression;
    opts.compressionOptions = compressionOptionsFromLevel(compression, compressionLevel);
    opts.mayOverwrite = mayOverwrite;
    opts.futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);
    opts.progressCallback = [weak = weak_from_this(), operationId, name = rpcName("onArchiveUploadProgress")](
                                auto min, auto max, auto current, auto bytesPerSecond
                            )
    {
        auto self = weak.lock();
        if (!self)
            return;
        self->within_strand_do(
            [weak, name, operationId, min, max, current, bytesPerSecond]()
            {
                auto self = weak.lock();
                if (!self)
                    return;
                RpcHelper::callRemoteSafely(
                    *self->hub_,
                    name,
                    SharedData::TransferProgress{
                        .operationId = operationId,
                        .min = min,
                        .max = max,
                        .current = current,
                        .bytesPerSecond = bytesPerSecond,
                    }
                );
            }
        );
    };

    auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;

    // Directory roots are scanned by the archive operation itself before it packs.
    auto archive = std::make_unique<ArchiveUploadOperation>(sftp, std::move(opts));
    for (auto const& rootPath : directoryRootPaths)
    {
        archive->addScanRoot(std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{
            .progressCallback = makeScanProgressCallback("onLocalScanProgress", operationId),
            .localPath = rootPath,
        }));
    }
    const auto scanPhase = archive->scanning();
    targetQueue.emplace_back(operationId, std::move(archive));

    Log::info(
        "OperationQueue::{}::onOperationAdded (ArchiveUpload, {} paths, ~{} bytes, {} pre-scans)",
        sessionId_.value(),
        rootCount,
        estimatedTotalBytes,
        directoryRootPaths.size()
    );
    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::ArchiveUpload,
            .mode = mode,
            .insertRefresh = true,
            .totalBytes = estimatedTotalBytes,
            .remotePath = remoteArchivePath,
            .scanPhase = scanPhase,
        }
    );
    return {};
}

std::size_t OperationQueue::addBulkDownloadOperation(
    SecureShell::SftpSession& sftp,
    SharedData::BulkDownloadRequest const& request,
    std::function<Ids::OperationId(std::size_t)> operationIdFor,
    Ids::OperationId bulkCardId
)
{
    // Assumed in strand.  The file portion is collapsed into a SINGLE
    // BulkDownloadOperation with prescanned entries — one card on the
    // frontend, one SSH-thread worker for the whole batch.  Directory
    // entries still use the existing Scan+BulkDownload flow (one card
    // per directory root).

    auto transferOptions = sftpOpts_.downloadOptions.value_or(Persistence::DownloadOptions{});
    if (request.allowOverwrite)
        transferOptions.mayOverwrite = true;
    const auto resolvedTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);
    auto& targetQueue = (request.mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;

    return splitBulkRequest(
        std::move(request),
        bulkCardId,
        [&](std::size_t index, SharedData::BulkDownloadEntry const& entry, SharedData::BulkDownloadRequest const& unit)
        {
            const auto result = addDownloadOperation(
                sftp,
                operationIdFor(idx),
                entry.destination,
                entry.source,
                request.allowOverwrite || entry.mayOverwrite,
                /*isBigFile*/ false,
                // Propagate the request's flag so manual folder downloads refresh
                // the local panel. Sync passes false to avoid refresh spam; a
                // manual bulk download passes true and expects one refresh per
                // top-level directory on completion.
                /*insertRefresh*/ request.insertRefresh,
                /*createMissingDirectories*/ true,
                request.mode
            );
            if (!result.has_value())
            {
                Log::error(
                    "Bulk download: directory entry '{}' failed to queue: {}",
                    entry.source.value(),
                    result.error().toString()
                );
                continue;
            }
            ++directoryCount;
            continue;
        }
        fileUnit.entries.push_back(entry);
        files.push_back(
            BulkDownloadOperation::PrescannedFile{
                .source = entry.source,
                .destination = entry.destination,
                .sizeBytes = entry.sizeBytes,
                .mtime = entry.mtime,
                .mtimeNsec = entry.mtimeNsec,
                .mayOverwrite = entry.mayOverwrite,
                .isDirectory = entry.isDirectory,
            }
        );
    }

    if (!files.empty())
    {
        // Dedicated operationId for the aggregate bulk-download card, reserved
        // by the frontend separately from the per-entry ids to avoid collisions
        // (entry-0's id would clash when entry 0 is a directory).
        const auto bulkOpId = bulkCardId;
        const auto remoteLabel = request.remoteRoot
            ? *request.remoteRoot
            : commonParentOf(files, &BulkDownloadOperation::PrescannedFile::source);
        const auto localLabel = request.localRoot
            ? *request.localRoot
            : commonParentOf(files, &BulkDownloadOperation::PrescannedFile::destination);
        fileUnit.remoteRoot = remoteLabel;
        fileUnit.localRoot = localLabel;

        auto downloadOpts = resolveDownloadOptions(transferOptions, resolvedTimeout);
        // Sync plans list files of directories that do not exist locally yet.
        downloadOpts.createMissingDirectories = true;

        auto bulk = std::make_unique<BulkDownloadOperation>(
            sftp,
            BulkDownloadOperation::BulkDownloadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkDownloadProgress", bulkOpId),
                .remotePath = remoteLabel,
                .localPath = localLabel,
                .individualOptions = std::move(downloadOpts),
                .failFast = transferOptions.failFast.value_or(false),
                .concurrency = bulkConcurrency_,
            }
        );
        bulk->setPrescannedFileList(std::move(files));

        targetQueue.emplace_back(bulkOpId, std::move(bulk));
        RpcHelper::callRemoteSafely(
            *hub_,
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = bulkOpId,
                .type = SharedData::OperationType::BulkDownload,
                .mode = request.mode,
                .insertRefresh = request.insertRefresh,
                .localPath = localLabel,
                .remotePath = remoteLabel,
            }
        );
    }

    Log::info("Bulk download: queued one bulk card (for files) + {} directory entries", directoryCount);

    // Stashed under the card id so a teardown can hand it to the resume registry.
    if (!fileUnit.entries.empty())
        bulkResumeStash_.insert_or_assign(bulkCardId, BulkResumeEntry{.request = std::move(fileUnit)});

    return request.entries.size();
}

std::expected<void, Operation::Error> OperationQueue::addUploadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    Paths::LocalPath const& localPath,
    Paths::RemotePath const& remotePath,
    bool allowOverwrite,
    bool isBigFile,
    bool insertRefresh,
    bool createMissingDirectories,
    SharedData::OperationMode mode
)
{
    // Assumed in strand

    const auto localNativePath = localPath.native();

    std::error_code ec;
    // lstat-style: do not follow links here so dangling symlinks can still be uploaded as
    // symlinks (UploadOperation::handleSymlink recreates the literal on the remote).
    const auto symStat = std::filesystem::symlink_status(localNativePath, ec);

    if (ec)
    {
        Log::error("Failed to stat local file '{}': {}", localPath.toUtf8(), Utility::errorMessage(ec));
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FileStatFailed});
    }

    const bool isSymlink = std::filesystem::is_symlink(symStat);
    std::filesystem::file_status stat = symStat;
    if (!isSymlink)
    {
        stat = std::filesystem::status(localNativePath, ec);
        if (ec)
        {
            Log::error("Failed to stat local file '{}': {}", localPath.toUtf8(), Utility::errorMessage(ec));
            return std::unexpected(Operation::Error{.type = Operation::ErrorType::FileStatFailed});
        }
    }

    auto transferOptions = sftpOpts_.uploadOptions.value_or(Persistence::UploadOptions{});
    if (allowOverwrite)
        transferOptions.mayOverwrite = allowOverwrite;

    const auto resolvedTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);

    // Symlinks always go through the single-file UploadOperation path so handleSymlink() can
    // recreate them on the remote — regardless of whether the target exists or is a directory.
    if (isSymlink || stat.type() == std::filesystem::file_type::regular)
    {
        auto opts = resolveUploadOptions(transferOptions, resolvedTimeout);
        opts.remotePath = remotePath;
        opts.localPath = localPath;
        opts.bigFileOptimized = isBigFile;
        opts.createMissingDirectories = createMissingDirectories;
        opts.progressCallback = [weak = weak_from_this(), operationId, name = rpcName("onUploadProgress")](
                                    auto min, auto max, auto current, auto bytesPerSecond
                                )
        {
            auto self = weak.lock();
            if (!self)
                return;
            self->within_strand_do(
                [weak, name, operationId, min, max, current, bytesPerSecond]()
                {
                    auto self = weak.lock();
                    if (!self)
                        return;
                    RpcHelper::callRemoteSafely(
                        *self->hub_,
                        name,
                        SharedData::TransferProgress{
                            .operationId = operationId,
                            .min = min,
                            .max = max,
                            .current = current,
                            .bytesPerSecond = bytesPerSecond,
                        }
                    );
                }
            );
        };

        auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
        targetQueue.emplace_back(operationId, std::make_unique<UploadOperation>(sftp, std::move(opts)));

        RpcHelper::callRemoteSafely(
            *hub_,
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::Upload,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .localPath = localPath,
                .remotePath = remotePath,
                .allowOverwrite = allowOverwrite,
            }
        );

        return {};
    }
    else if (stat.type() == std::filesystem::file_type::directory)
    {
        auto bulk = std::make_unique<BulkUploadOperation>(
            sftp,
            BulkUploadOperation::BulkUploadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkUploadProgress", operationId),
                .remotePath = remotePath,
                .localPath = localPath,
                .individualOptions = resolveUploadOptions(transferOptions, resolvedTimeout),
                .failFast = transferOptions.failFast.value_or(false),
                .concurrency = bulkConcurrency_,
            }
        );
        // The scan is part of the bulk, so cancelling or moving the card acts on both.
        bulk->scanFirst(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{
                .progressCallback = makeScanProgressCallback("onLocalScanProgress", operationId),
                .localPath = localPath,
            })
        );

        auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
        targetQueue.emplace_back(operationId, std::move(bulk));

        RpcHelper::callRemoteSafely(
            *hub_,
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::BulkUpload,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .localPath = localPath,
                .remotePath = remotePath,
                .scanPhase = true,
            }
        );

        // Synthetic single-entry resume backup: adopting it walks the same scan and bulk path.
        SharedData::BulkUploadRequest synthetic{
            .entries = {SharedData::BulkUploadEntry{
                .source = localPath, .destination = remotePath, .isDirectory = true
            }},
            .allowOverwrite = allowOverwrite,
            .insertRefresh = insertRefresh,
            .mode = mode,
        };
        bulkResumeStash_.insert_or_assign(operationId, BulkResumeEntry{.request = std::move(synthetic)});

        return {};
    }
    else
    {
        Log::error("Local path is neither a file nor a directory: {}.", static_cast<std::uint8_t>(stat.type()));
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::OperationNotPossibleOnFileType});
    }
}

std::size_t OperationQueue::addBulkUploadOperation(
    SecureShell::SftpSession& sftp,
    SharedData::BulkUploadRequest const& request,
    std::function<Ids::OperationId(std::size_t)> operationIdFor,
    Ids::OperationId bulkCardId
)
{
    // Mirrors addBulkDownloadOperation — file portion collapses into one
    // BulkUploadOperation card; directory entries reuse the existing
    // LocalScan+BulkUpload flow.
    auto transferOptions = sftpOpts_.uploadOptions.value_or(Persistence::UploadOptions{});
    if (request.allowOverwrite)
        transferOptions.mayOverwrite = true;
    const auto resolvedTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);
    auto& targetQueue = (request.mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;

    return splitBulkRequest(
        std::move(request),
        bulkCardId,
        [&](std::size_t index, SharedData::BulkUploadEntry const& entry, SharedData::BulkUploadRequest const& unit)
        {
            const auto result = addUploadOperation(
                sftp,
                operationIdFor(idx),
                entry.source,
                entry.destination,
                request.allowOverwrite || entry.mayOverwrite,
                /*isBigFile*/ false,
                // Propagate the request's flag so manual folder uploads refresh
                // the remote panel. Sync passes false to avoid refresh spam; a
                // manual bulk upload passes true and expects one refresh per
                // top-level directory on completion.
                /*insertRefresh*/ request.insertRefresh,
                /*createMissingDirectories*/ true,
                request.mode
            );
            if (!result.has_value())
            {
                Log::error(
                    "Bulk upload: directory entry '{}' failed to queue: {}",
                    entry.source.toUtf8(),
                    result.error().toString()
                );
                continue;
            }
            ++directoryCount;
            continue;
        }
        fileUnit.entries.push_back(entry);
        files.push_back(
            BulkUploadOperation::PrescannedFile{
                .source = entry.source,
                .destination = entry.destination,
                .sizeBytes = entry.sizeBytes,
                .mayOverwrite = entry.mayOverwrite,
                .isDirectory = entry.isDirectory,
            }
        );
    }

    if (!files.empty())
    {
        // Separate reserved id — see addBulkDownloadOperation for rationale.
        const auto bulkOpId = bulkCardId;
        const auto localLabel = request.localRoot ? *request.localRoot
                                                  : commonParentOf(files, &BulkUploadOperation::PrescannedFile::source);
        const auto remoteLabel = request.remoteRoot
            ? *request.remoteRoot
            : commonParentOf(files, &BulkUploadOperation::PrescannedFile::destination);
        fileUnit.remoteRoot = remoteLabel;
        fileUnit.localRoot = localLabel;

        auto uploadOpts = resolveUploadOptions(transferOptions, resolvedTimeout);
        uploadOpts.createMissingDirectories = true;

        auto bulk = std::make_unique<BulkUploadOperation>(
            sftp,
            BulkUploadOperation::BulkUploadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkUploadProgress", bulkOpId),
                .remotePath = remoteLabel,
                .localPath = localLabel,
                .individualOptions = std::move(uploadOpts),
                .failFast = transferOptions.failFast.value_or(false),
                .concurrency = bulkConcurrency_,
            }
        );
        bulk->setPrescannedFileList(std::move(files));

        targetQueue.emplace_back(bulkOpId, std::move(bulk));
        RpcHelper::callRemoteSafely(
            *hub_,
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = bulkOpId,
                .type = SharedData::OperationType::BulkUpload,
                .mode = request.mode,
                .insertRefresh = request.insertRefresh,
                .localPath = localLabel,
                .remotePath = remoteLabel,
            }
        );
    }

    Log::info("Bulk upload: queued one bulk card (for files) + {} directory entries", directoryCount);

    if (!fileUnit.entries.empty())
        bulkResumeStash_.insert_or_assign(bulkCardId, BulkResumeEntry{.request = std::move(fileUnit)});

    return request.entries.size();
}

SharedData::Sync::SyncExecutionResult OperationQueue::executeSyncPlan(
    SecureShell::SftpSession& sftp,
    SyncSession const& session,
    SyncSession::ResolvedPlan plan,
    SharedData::Sync::SyncExecutionRequest const& request
)
{
    // Assumed in strand
    using SharedData::Sync::Action;
    SharedData::Sync::SyncExecutionResult result{.failed = std::move(plan.refused)};

    SharedData::BulkUploadRequest uploads{
        .mode = request.mode,
        .localRoot = session.localRoot(),
        .remoteRoot = session.remoteRoot(),
    };
    SharedData::BulkDownloadRequest downloads{
        .mode = request.mode,
        .localRoot = session.localRoot(),
        .remoteRoot = session.remoteRoot(),
    };
    SharedData::BulkDeleteRequest remoteDeletes{.mode = request.mode, .listedContentOnly = true};
    std::vector<Paths::LocalPath> localDeletes;
    const auto localHomes = localHomeDirectories();
    auto remoteGuard = remoteDeleteGuard(sftp);
    remoteGuard.trustScannedBelow(session.remoteRoot());

    // The plan lists a directory before its content, so a refused directory refuses every entry below it.
    std::vector<std::string> refusedRemoteDirectories;
    std::vector<std::string> refusedLocalDirectories;
    const auto isBelowAny = [](std::vector<std::string> const& directories, std::string const& relKey)
    {
        return std::ranges::any_of(
            directories,
            [&relKey](std::string const& directory)
            {
                return relKey.size() > directory.size() && relKey.starts_with(directory) &&
                    relKey[directory.size()] == '/';
            }
        );
    };
    const auto refuse = [&result](
                            SharedData::Sync::EnqueuePlanEntry& entry,
                            std::vector<std::string>& refusedDirectories,
                            std::string_view side,
                            std::string const& reason
                        )
    {
        Log::error("Sync: refusing to delete {} '{}' and what is below it: {}", side, entry.relKey, reason);
        if (entry.isDirectory)
            refusedDirectories.push_back(entry.relKey);
        result.failed.push_back(std::move(entry.relKey));
    };

    auto const& pairKey = session.baselineKey();
    auto const& baselines = session.baselines();
    const auto makeRecording = [&]()
    {
        return SyncRecording{.pairKey = pairKey.value_or(std::string{}), .store = baselines};
    };
    auto uploadRecording = makeRecording();
    auto downloadRecording = makeRecording();
    auto remoteDeleteRecording = makeRecording();
    auto localDeleteRecording = makeRecording();

    for (auto& planned : plan.entries)
    {
        auto& entry = planned.entry;
        auto& paths = planned.paths;
        switch (entry.action)
        {
            case Action::Upload:
            {
                uploadRecording.entries.emplace_back(entry.relKey, std::move(planned.record));
                uploads.entries.push_back({
                    .source = std::move(paths.local),
                    .destination = std::move(paths.remote),
                    .sizeBytes = entry.sizeBytes,
                    .isDirectory = entry.isDirectory,
                    .withoutContent = entry.isDirectory,
                    .mayOverwrite = planned.compared.exists,
                    .comparedDestination = comparedDestination,
                });
                result.uploads.push_back(std::move(entry.relKey));
                break;
            }
            case Action::Download:
            {
                downloadRecording.entries.emplace_back(entry.relKey, std::move(planned.record));
                downloads.entries.push_back({
                    .source = std::move(paths.remote),
                    .destination = std::move(paths.local),
                    .sizeBytes = entry.sizeBytes,
                    .isDirectory = entry.isDirectory,
                    .withoutContent = entry.isDirectory,
                    .mtime = entry.mtime,
                    .mtimeNsec = entry.mtimeNsec,
                });
                result.downloads.push_back(std::move(entry.relKey));
                break;
            }
            case Action::DeleteRemote:
            {
                if (isBelowAny(refusedRemoteDirectories, entry.relKey))
                {
                    result.failed.push_back(std::move(entry.relKey));
                    break;
                }
                if (auto allowed = remoteGuard.check(paths.remote); !allowed)
                {
                    refuse(entry, refusedRemoteDirectories, "remotely", allowed.error().toString());
                    break;
                }
                remoteDeleteRecording.entries.emplace_back(entry.relKey, std::nullopt);
                remoteDeleteRecording.sources.push_back(paths.remote.value());
                result.remoteDeleteSources.push_back(paths.remote.value());
                remoteDeletes.entries.push_back({
                    .target = std::move(paths.remote),
                    .isDirectory = entry.isDirectory,
                    .compared = std::move(planned.compared),
                });
                result.remoteDeletes.push_back(std::move(entry.relKey));
                break;
            }
            case Action::DeleteLocal:
            {
                if (isBelowAny(refusedLocalDirectories, entry.relKey))
                {
                    result.failed.push_back(std::move(entry.relKey));
                    break;
                }
                if (auto allowed = refuseProtectedLocalDelete(paths.local, localHomes); !allowed)
                {
                    refuse(entry, refusedLocalDirectories, "locally", allowed.error());
                    break;
                }
                localDeleteRecording.entries.emplace_back(entry.relKey, std::nullopt);
                localDeleteRecording.sources.push_back(paths.local.toUtf8());
                result.localDeleteSources.push_back(paths.local.toUtf8());
                localDeletes.push_back(std::move(paths.local));
                result.localDeletes.push_back(std::move(entry.relKey));
                break;
            }
        }
    }

    if (!uploads.entries.empty())
    {
        addBulkUploadOperation(
            sftp,
            uploads,
            [](std::size_t)
            {
                return Ids::generateOperationId();
            },
            request.uploadId
        );
    }
    if (!downloads.entries.empty())
    {
        addBulkDownloadOperation(
            sftp,
            downloads,
            [](std::size_t)
            {
                return Ids::generateOperationId();
            },
            request.downloadId
        );
    }
    if (!remoteDeletes.entries.empty() &&
        addBulkDeleteOperation(remoteGuard, sftp, remoteDeletes, request.remoteDeleteId) == 0)
    {
        std::ranges::move(result.remoteDeletes, std::back_inserter(result.failed));
        result.remoteDeletes.clear();
        result.remoteDeleteSources.clear();
    }
    if (!localDeletes.empty() &&
        !localDeleteQueue.addDelete({
            .operationId = request.localDeleteId,
            .targets = std::move(localDeletes),
            .mode = request.mode,
            .owner = sessionId_,
            .held = paused_,
            .comparedStates = std::move(localDeletesCompared),
        }))
    {
        std::ranges::move(result.localDeletes, std::back_inserter(result.failed));
        result.localDeletes.clear();
        result.localDeleteSources.clear();
    }

    if (pairKey && baselines)
    {
        baselines->update(
            *pairKey,
            [&plan](SharedData::Sync::SyncBaseline& baseline)
            {
                for (auto& [relKey, record] : plan.agreement.records)
                    baseline.records.insert_or_assign(relKey, std::move(record));
            }
        );
        const auto keep = [this](Ids::OperationId const& operationId, bool queued, SyncRecording recording)
        {
            if (queued && !recording.entries.empty())
                syncRecordings_.insert_or_assign(operationId.value(), std::move(recording));
        };
        keep(request.uploadId, !result.uploads.empty(), std::move(uploadRecording));
        keep(request.downloadId, !result.downloads.empty(), std::move(downloadRecording));
        keep(request.remoteDeleteId, !result.remoteDeletes.empty(), std::move(remoteDeleteRecording));
        keep(request.localDeleteId, !result.localDeletes.empty(), std::move(localDeleteRecording));
    }

    Log::info(
        "Sync: queued {} uploads, {} downloads, {} remote and {} local deletes, {} failed",
        result.uploads.size(),
        result.downloads.size(),
        result.remoteDeletes.size(),
        result.localDeletes.size(),
        result.failed.size()
    );
    return result;
}

std::expected<void, Operation::Error> OperationQueue::addLocalDeleteOperation(
    Ids::OperationId operationId,
    std::vector<Paths::LocalPath> targets,
    bool recursive,
    bool insertRefresh,
    SharedData::OperationMode mode,
    std::vector<SharedData::ComparedState> comparedStates
)
{
    // Assumed in strand
    if (targets.empty())
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::InvalidPath, .extraInfo = "no targets"});
    const auto homes = localHomeDirectories();
    for (auto const& target : targets)
    {
        if (auto allowed = refuseProtectedLocalDelete(target, homes); !allowed)
        {
            Log::error("Refusing to delete locally: {}", allowed.error());
            return std::unexpected(Operation::Error{.type = Operation::ErrorType::InvalidPath, .extraInfo = target.toUtf8()});
        }
    }

    auto parent = LocalDeleteOperation::commonParentOf(targets);
    auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
    targetQueue.emplace_back(
        operationId,
        std::make_unique<LocalDeleteOperation>(LocalDeleteOperation::LocalDeleteOperationOptions{
            .itemsRemovedProgress = makeDeleteProgressCallback(operationId),
            .scanProgress =
                [progress = makeScanProgressCallback("onScanProgress", operationId)](
                    std::uint64_t bytes, std::uint64_t entries
                )
            {
                progress(bytes, entries, entries);
            },
            .targets = std::move(targets),
            .recursive = recursive,
        })
    );

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::LocalDelete,
            .mode = mode,
            .insertRefresh = insertRefresh,
            .localPath = std::move(parent),
            .scanPhase = recursive,
        }
    );
    return {};
}

void OperationQueue::cancelLocalDeletes()
{
    // Assumed in strand
    for (auto* queue : {&priorityOperations_, &operations_})
    {
        std::erase_if(
            *queue,
            [this](auto& entry)
            {
                auto& [id, operation] = entry;
                if (operation->type() != SharedData::OperationType::LocalDelete)
                    return false;
                std::ignore = operation->cancel(true);
                completeOperation(makeCompletedOperation(CompletionReason::Canceled, id, *operation));
                return true;
            }
        );
    }
}

RemoteDeleteGuard OperationQueue::remoteDeleteGuard(SecureShell::SftpSession& sftp)
{
    return RemoteDeleteGuard{sftp, remoteHome_, sftpOpts_.operationTimeout.value_or(defaultFutureTimeout)};
}

std::expected<void, Operation::Error> OperationQueue::addDeleteOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    Paths::RemotePath const& remotePath,
    bool recursive,
    bool insertRefresh,
    SharedData::OperationMode mode
)
{
    auto guard = remoteDeleteGuard(sftp);
    return addDeleteOperation(guard, sftp, std::move(operationId), remotePath, recursive, insertRefresh, mode);
}

std::expected<void, Operation::Error> OperationQueue::addDeleteOperation(
    RemoteDeleteGuard& guard,
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    Paths::RemotePath const& remotePath,
    bool recursive,
    bool insertRefresh,
    SharedData::OperationMode mode
)
{
    // Assumed in strand
    auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;

    if (auto allowed = guard.check(remotePath); !allowed)
    {
        Log::error("Refusing to delete: {}", allowed.error().toString());
        return std::unexpected(
            Operation::Error{.type = Operation::ErrorType::InvalidPath, .extraInfo = allowed.error().toString()}
        );
    }

    // lstat: a symlink to a directory is removed as a link, recursing would delete the target's content.
    auto statFut = sftp.lstat(remotePath);
    if (statFut.wait_for(sftpOpts_.operationTimeout.value_or(defaultFutureTimeout)) != std::future_status::ready)
    {
        Log::error("addDeleteOperation: lstat timed out for '{}'", remotePath.value());
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FutureTimeout});
    }
    const auto statResult = statFut.get();
    const bool isDirectory = statResult.has_value() && statResult->isDirectory();
    const auto isBulk = recursive && isDirectory;

    auto operation = std::make_unique<DeleteOperation>(
        sftp,
        DeleteOperation::DeleteOperationOptions{
            .filesRemovedProgress = makeDeleteProgressCallback(operationId),
            .remotePath = remotePath,
            .futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout),
            .recursive = isBulk,
        }
    );
    if (isBulk)
    {
        // The scan is part of the delete, so cancelling or moving the card acts on both.
        operation->addScanRoot(
            std::make_unique<ScanOperation>(
                sftp,
                ScanOperation::ScanOperationOptions{
                    .progressCallback = makeScanProgressCallback("onScanProgress", operationId),
                    .remotePath = remotePath,
                    .futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout),
                }
            )
        );
    }

    targetQueue.emplace_back(operationId, std::move(operation));

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::Delete,
            .mode = mode,
            .insertRefresh = insertRefresh,
            .remotePath = remotePath,
            .scanPhase = isBulk,
        }
    );

    return {};
}

std::size_t OperationQueue::addBulkDeleteOperation(
    SecureShell::SftpSession& sftp,
    SharedData::BulkDeleteRequest const& request,
    Ids::OperationId bulkOperationId
)
{
    auto guard = remoteDeleteGuard(sftp);
    return addBulkDeleteOperation(guard, sftp, request, std::move(bulkOperationId));
}

std::size_t OperationQueue::addBulkDeleteOperation(
    RemoteDeleteGuard& guard,
    SecureShell::SftpSession& sftp,
    SharedData::BulkDeleteRequest const& request,
    Ids::OperationId bulkOperationId
)
{
    // Assumed in strand. One DeleteOperation, so one card and one completion for the whole request: files are
    // deleted as given, every real directory is scanned first and deleted with its content.
    auto& targetQueue = (request.mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
    const auto resolvedTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);

    auto operation = std::make_unique<DeleteOperation>(
        sftp,
        DeleteOperation::DeleteOperationOptions{
            .filesRemovedProgress = makeDeleteProgressCallback(bulkOperationId),
            .futureTimeout = resolvedTimeout,
            .recursive = true,
        }
    );

    std::vector<Paths::RemotePath> fileTargets;
    std::size_t scannedDirectories = 0;
    std::size_t failedTargets = 0;
    const auto failTarget = [&](Paths::RemotePath const& target, Operation::Error error)
    {
        operation->addFailedTarget(target, std::move(error));
        ++failedTargets;
    };
    // The roots are scanned one after another, their counts add up on the card.
    const auto scanProgressOfNextRoot =
        Detail::sequentialScanProgress(makeScanProgressCallback("onScanProgress", bulkOperationId));
    for (auto const& entry : request.entries)
    {
        if (auto allowed = guard.check(entry.target); !allowed)
        {
            Log::error("Bulk delete: refusing to delete: {}", allowed.error().toString());
            failTarget(
                entry.target,
                Operation::Error{.type = Operation::ErrorType::InvalidPath, .extraInfo = allowed.error().toString()}
            );
            continue;
        }
        if (entry.isDirectory && !request.listedContentOnly)
        {
            // lstat: a symlink to a directory is removed as a link, recursing would delete the target's content.
            auto statFuture = sftp.lstat(entry.target);
            if (statFuture.wait_for(resolvedTimeout) != std::future_status::ready)
            {
                Log::error("Bulk delete: lstat timed out for '{}'", entry.target.value());
                failTarget(entry.target, Operation::Error{.type = Operation::ErrorType::FutureTimeout});
                continue;
            }
            const auto information = statFuture.get();
            if (information && information->isDirectory())
            {
                operation->addScanRoot(
                    std::make_unique<ScanOperation>(
                        sftp,
                        ScanOperation::ScanOperationOptions{
                            .progressCallback = scanProgressOfNextRoot(),
                            .remotePath = entry.target,
                            .futureTimeout = resolvedTimeout,
                        }
                    )
                );
                ++scannedDirectories;
                continue;
            }
        }
        fileTargets.push_back(entry.target);
    }

    const auto queued = fileTargets.size() + scannedDirectories;
    if (queued == 0)
        return 0;
    if (failedTargets > 0)
        Log::warn("Bulk delete: {} items were refused, they are reported as failed", failedTargets);

    // Parent of the first item, so the card can read "Deleting items in <dir>" before any progress arrives.
    const auto commonParent = request.entries.front().target.parent().value_or(Paths::RemotePath::root());
    const auto fileCount = fileTargets.size();
    operation->setTargets(std::move(fileTargets));
    targetQueue.emplace_back(bulkOperationId, std::move(operation));

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = bulkOperationId,
            .type = SharedData::OperationType::BulkDelete,
            .mode = request.mode,
            .insertRefresh = request.insertRefresh,
            .totalBytes = static_cast<std::uint64_t>(fileCount),
            .remotePath = commonParent,
            .scanPhase = scannedDirectories > 0,
        }
    );
    Log::info("Bulk delete: {} files and {} scanned directories in one operation", fileCount, scannedDirectories);

    if (!request.entries.empty())
    {
        bulkResumeStash_.insert_or_assign(bulkOperationId, BulkResumeEntry{.request = request});
    }

    return queued;
}

std::expected<void, Operation::Error> OperationQueue::addRenameOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    Paths::RemotePath const& sourcePath,
    Paths::RemotePath const& destinationPath,
    SharedData::OperationMode mode
)
{
    // Assumed in strand

    auto operation = std::make_unique<RenameOperation>(
        sftp,
        RenameOperation::RenameOperationOptions{
            .sourcePath = sourcePath,
            .destinationPath = destinationPath,
            .futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout),
        }
    );

    auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
    targetQueue.emplace_back(operationId, std::move(operation));

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::Rename,
            .mode = mode,
            .source = sourcePath,
            .destination = destinationPath,
        }
    );

    return {};
}

void OperationQueue::addSyncScanOperation(
    SecureShell::SftpSession& sftp,
    Ids::SyncSessionId syncSessionId,
    Ids::OperationId remoteScanId,
    Ids::OperationId localScanId,
    Paths::RemotePath const& remotePath,
    Paths::LocalPath const& localPath,
    bool respectIgnoreFiles,
    bool recursive,
    bool ignoreHidden,
    std::optional<std::string> const& sessionName
)
{
    // Create (or replace) the SyncSession.  A fresh id from the frontend is the
    // norm; re-opening over an existing id is handled as "discard previous state".
    const bool keepsBaseline = sessionName && !sessionName->empty() && syncBaselines_;
    auto session = std::make_shared<SyncSession>(
        SyncSession::Options{
            .sessionId = syncSessionId,
            .localRoot = localPath,
            .remoteRoot = remotePath,
            .baselineKey = keepsBaseline
                ? std::optional{SyncBaselineStore::pairKey(*sessionName, localPath, remotePath)}
                : std::nullopt,
            .baselines = keepsBaseline ? syncBaselines_ : nullptr,
        },
        executor_
    );
    syncSessions_[syncSessionId.value()] = session;

    syncScanCallbacks_[remoteScanId.value()] = makeSyncScanCallback(
        syncSessionId, session, localScanId, /*isLocal=*/false, &SyncSession::setRemoteTreeInStrand
    );
    syncScanCallbacks_[localScanId.value()] = makeSyncScanCallback(
        syncSessionId, session, remoteScanId, /*isLocal=*/true, &SyncSession::setLocalTreeInStrand
    );

    // Queue remote scan — build-tree mode.
    // Temp files of transfers on either side and the asides of replaced destinations, a sync neither copies nor
    // deletes them.
    const auto uploadSuffix = Persistence::effectiveTempFileSuffix(
        sftpOpts_.uploadOptions.value_or(Persistence::UploadOptions{}).tempFileSuffix
    );
    const auto downloadSuffix = Persistence::effectiveTempFileSuffix(
        sftpOpts_.downloadOptions.value_or(Persistence::DownloadOptions{}).tempFileSuffix
    );
    std::vector<std::string> temporarySuffixes{
        uploadSuffix,
        downloadSuffix,
        Persistence::asideFileSuffix(uploadSuffix),
        Persistence::asideFileSuffix(downloadSuffix),
    };

    auto remoteScan = std::make_unique<ScanOperation>(
        sftp,
        ScanOperation::ScanOperationOptions{
            .progressCallback = makeScanProgressCallback("onScanProgress", remoteScanId),
            .remotePath = remotePath,
            .futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout),
            .respectIgnoreFiles = respectIgnoreFiles,
            .recursive = recursive,
            .ignoreHidden = ignoreHidden,
            .buildTree = true,
            .excludedSuffixes = temporarySuffixes,
        }
    );
    priorityOperations_.emplace_back(remoteScanId, std::move(remoteScan));

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = remoteScanId,
            .type = SharedData::OperationType::Scan,
            .mode = SharedData::OperationMode::PriorityQueued,
            .remotePath = remotePath,
        }
    );

    // Queue local scan — build-tree mode.
    auto localScan = std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{
        .progressCallback = makeScanProgressCallback("onLocalScanProgress", localScanId),
        .localPath = localPath,
        .respectIgnoreFiles = respectIgnoreFiles,
        .recursive = recursive,
        .ignoreHidden = ignoreHidden,
        .buildTree = true,
        .excludedSuffixes = std::move(temporarySuffixes),
    });
    priorityOperations_.emplace_back(localScanId, std::move(localScan));

    RpcHelper::callRemoteSafely(
        *hub_,
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = localScanId,
            .type = SharedData::OperationType::LocalScan,
            .mode = SharedData::OperationMode::PriorityQueued,
            .localPath = localPath,
        }
    );
}

void OperationQueue::setSyncBaselineStore(std::shared_ptr<SyncBaselineStore const> store)
{
    syncBaselines_ = std::move(store);
}

void OperationQueue::recordSyncCompletion(OperationCompleted const& completed)
{
    auto found = syncRecordings_.extract(completed.operationId.value());
    if (found.empty() || completed.reason != SharedData::OperationCompletionReason::Completed)
        return;
    auto& recording = found.mapped();

    std::vector<bool> failed(recording.entries.size(), false);
    for (const auto index : completed.failedEntryIndices)
    {
        if (index < failed.size())
            failed[index] = true;
    }
    if (!recording.sources.empty() && !completed.failedEntries.empty())
    {
        std::unordered_set<std::string> failedSources;
        for (auto const& [source, error] : completed.failedEntries)
            failedSources.insert(source);
        for (std::size_t index = 0; index < recording.sources.size() && index < failed.size(); ++index)
            failed[index] = failed[index] || failedSources.contains(recording.sources[index]);
    }

    recording.store->update(
        recording.pairKey,
        [&recording, &failed](SharedData::Sync::SyncBaseline& baseline)
        {
            const SharedData::Sync::SyncRecord directory{
                .local = {.type = SharedData::FileType::Directory},
                .remote = {.type = SharedData::FileType::Directory},
            };
            for (std::size_t index = 0; index < recording.entries.size(); ++index)
            {
                if (failed[index])
                    continue;
                auto const& [relKey, record] = recording.entries[index];
                if (!record)
                {
                    baseline.records.erase(relKey);
                    continue;
                }
                baseline.records.insert_or_assign(relKey, *record);
                // The transfer created or found every directory above it on both sides.
                for (
                    auto slash = relKey.rfind('/'); slash != std::string::npos && slash > 0;
                    slash = relKey.rfind('/', slash - 1)
                )
                    baseline.records.insert_or_assign(relKey.substr(0, slash), directory);
            }
        }
    );
}

std::shared_ptr<SyncSession> OperationQueue::syncSession(Ids::SyncSessionId const& sessionId) const
{
    const auto iter = syncSessions_.find(sessionId.value());
    return iter == syncSessions_.end() ? nullptr : iter->second;
}

void OperationQueue::closeSyncSession(Ids::SyncSessionId const& sessionId)
{
    const auto iter = syncSessions_.find(sessionId.value());
    if (iter == syncSessions_.end())
        return;
    if (iter->second)
        iter->second->cancel();
    syncSessions_.erase(iter);
}

void OperationQueue::registerRpc()
{
    on(rpcName("isPaused"))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply)
            {
                auto self = weak.lock();
                if (!self)
                    return reply(SharedData::error("OperationQueue no longer exists"));

                return reply(
                    SharedData::ErrorOrSuccess{SharedData::IsPaused{
                        .paused = self->paused(),
                    }}
                );
            }
        );

    on(rpcName("cancel"))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, Ids::OperationId operationId)
            {
                auto self = weak.lock();
                if (!self)
                    return reply(SharedData::error("OperationQueue no longer exists"));

                self->cancel(std::move(operationId));
                return reply(SharedData::success());
            }
        );

    on(rpcName("cancelAll"))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply)
            {
                auto self = weak.lock();
                if (!self)
                    return reply(SharedData::error("OperationQueue no longer exists"));

                self->cancelAll();
                return reply(SharedData::success());
            }
        );

    on(rpcName("moveOperation"))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, Ids::OperationId operationId, std::int32_t newIndex)
            {
                auto self = weak.lock();
                if (!self)
                    return reply(SharedData::error("OperationQueue no longer exists"));

                // Reply immediately to release the frontend's backchannel temp
                // RPC name. The actual move + onOperationsReordered broadcast
                // happen asynchronously inside moveOperation's strand task.
                const auto clamped = newIndex < 0 ? std::size_t{0} : static_cast<std::size_t>(newIndex);
                self->moveOperation(std::move(operationId), clamped);
                return reply(SharedData::success());
            }
        );
}
