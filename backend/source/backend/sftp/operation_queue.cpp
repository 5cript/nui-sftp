#include <backend/sftp/operation_queue.hpp>
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

using namespace std::chrono_literals;

namespace
{
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
                        .localPath = op.localPath(),
                        .remotePath = op.remotePath(),
                        .error = error,
                    };
                },
                [reason, operationId, error](ScanOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .remotePath = op.remotePath(),
                        .error = error,
                    };
                },
                [reason, operationId, error](LocalScanOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .localPath = op.localPath(),
                        .error = error,
                    };
                },
                [reason, operationId, error](BulkDownloadOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .error = error,
                        .failedEntries = op.getFailed(),
                    };
                },
                [reason, operationId, error](UploadOperation const&)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                    };
                },
                [reason, operationId, error](BulkUploadOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .error = error,
                        .failedEntries = op.getFailed(),
                    };
                },
                [reason, operationId, error](DeleteOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .remotePath = op.remotePath(),
                        .error = error,
                    };
                },
                [reason, operationId, error](RenameOperation const& op)
                {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .localPath = op.sourcePath(),
                        .remotePath = op.destinationPath(),
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
    , parallelism_{parallelism}
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
                self->hub_->callRemote(
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

auto OperationQueue::makeBulkProgressCallback(std::string_view eventName, Ids::OperationId operationId)
    -> std::function<void(
        std::filesystem::path const&,
        std::uint64_t,
        std::uint64_t,
        std::uint64_t,
        std::uint64_t,
        std::uint64_t,
        std::uint64_t,
        std::make_signed_t<std::size_t>
    )>
{
    return [weak = weak_from_this(), name = rpcName(eventName), operationId](
               std::filesystem::path const& currentFile,
               std::uint64_t fileCurrentIndex,
               std::uint64_t fileCount,
               std::uint64_t currentFileBytes,
               std::uint64_t currentFileTotalBytes,
               std::uint64_t bytesCurrent,
               std::uint64_t bytesTotal,
               std::make_signed_t<std::size_t> bytesPerSecond
           )
    {
        auto self = weak.lock();
        if (!self)
            return;
        self->within_strand_do(
            [weak,
                name,
                operationId,
                currentFile,
                fileCurrentIndex,
                fileCount,
                currentFileBytes,
                currentFileTotalBytes,
                bytesCurrent,
                bytesTotal,
                bytesPerSecond]()
            {
                auto self = weak.lock();
                if (!self)
                    return;
                self->hub_->callRemote(
                    name,
                    SharedData::BulkProgress{
                        .operationId = operationId,
                        .currentFile = currentFile.string(),
                        .fileCurrentIndex = fileCurrentIndex,
                        .fileCount = fileCount,
                        .currentFileBytes = currentFileBytes,
                        .currentFileTotalBytes = currentFileTotalBytes,
                        .bytesCurrent = bytesCurrent,
                        .bytesTotal = bytesTotal,
                        .bytesPerSecond = bytesPerSecond,
                    }
                );
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
                self->hub_->callRemote(
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

            self->hub_->callRemote(self->rpcName("onOperationCompleted"), std::move(operationCompleted));
        }
    );
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
        "OperationQueue::adoptBulkResume: re-issuing bulk operation '{}' (kind={}, {} entries)",
        operationId.value(),
        static_cast<int>(entry.kind),
        entry.request.entries.size()
    );

    // For bulk download/upload the frontend reserves one extra id at the
    // back of the per-entry id list for the aggregate card.  On adopt we
    // reuse the original aggregate id (so the surviving frontend card
    // re-attaches) and synthesise fresh per-entry ids since their cards
    // no longer exist in the new session.
    auto operationIdFor = [](std::size_t)
    {
        return Ids::generateOperationId();
    };

    switch (entry.kind)
    {
        case BulkResumeEntry::Kind::BulkDownload:
            addBulkDownloadOperation(sftp, entry.request, operationIdFor, operationId);
            break;
        case BulkResumeEntry::Kind::BulkUpload:
            addBulkUploadOperation(sftp, entry.request, operationIdFor, operationId);
            break;
        case BulkResumeEntry::Kind::BulkDelete:
            addBulkDeleteOperation(sftp, entry.request, operationId);
            break;
    }
}

void OperationQueue::deepPause(bool pause)
{
    const auto updateCount = std::min(operations_.size(), static_cast<std::size_t>(parallelism_));

    if (updateCount == 0)
        return;

    bool previousWasBarrier = false;
    for (std::size_t i = 0; i < updateCount; ++i)
    {
        if (previousWasBarrier)
            break;

        auto& [id, operation] = operations_[i];
        previousWasBarrier = operation->isBarrier();
        operation->pause(pause);
    }
}

bool OperationQueue::workQueue(std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>>& queue)
{
    // Assumed in (asio) strand.

    const auto updateCount = std::min(queue.size(), static_cast<std::size_t>(parallelism_));

    if (updateCount == 0)
        return false;

    // Collect the indices we are allowed to advance this cycle — barrier ops stop the run
    // before the next iteration.
    std::vector<std::size_t> eligible;
    eligible.reserve(updateCount);
    {
        bool previousWasBarrier = false;
        for (std::size_t idx = 0; idx < updateCount; ++idx)
        {
            if (previousWasBarrier)
                break;
            eligible.push_back(idx);
            previousWasBarrier = queue[idx].second->isBarrier();
        }
    }
    if (eligible.empty())
        return false;

    // Any strand-using op lets us share a single SFTP processing-thread umbrella across
    // every eligible op's workInStrand() call.  Pure-local ops run directly on the caller.
    SecureShell::ProcessingStrand* strand = nullptr;
    for (const auto idx : eligible)
    {
        auto* operation = queue[idx].second.get();
        if (operation->usesStrand())
        {
            strand = operation->strand();
            if (strand)
                break;
        }
    }

    struct ItemResult
    {
        std::size_t queueIndex;
        std::expected<Operation::WorkStatus, Operation::Error> result;
    };

    auto runBatch = [&queue, &eligible]()
    {
        std::vector<ItemResult> out;
        out.reserve(eligible.size());
        for (const auto idx : eligible)
        {
            auto* operation = queue[idx].second.get();
            auto res = operation->usesStrand() ? operation->workInStrand() : operation->work();
            const bool terminal = !res.has_value() || res.value() == Operation::WorkStatus::Complete;
            out.push_back({idx, std::move(res)});
            // Match the single-completion-per-workQueue-cycle semantics of the original
            // sequential driver: stop as soon as anybody completes or fails so subsequent
            // ops see the resulting queue mutation on the next cycle.
            if (terminal)
                break;
        }
        return out;
    };

    std::vector<ItemResult> results;
    if (strand)
    {
        auto fut = strand->pushPromiseTask(runBatch);
        results = fut.get();
    }
    else
    {
        results = runBatch();
    }

    bool moreWork = false;
    for (auto& item : results)
    {
        auto& [id, operation] = queue[item.queueIndex];

        if (!item.result.has_value())
        {
            completeOperation(
                makeCompletedOperation(OperationQueue::CompletionReason::Failed, id, *operation, item.result.error())
            );
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(item.queueIndex));
            return true;
        }

        const auto workStatus = item.result.value();
        if (workStatus == Operation::WorkStatus::Complete)
        {
            auto* next = (item.queueIndex + 1 < queue.size()) ? queue[item.queueIndex + 1].second.get() : nullptr;
            if (operation->type() == SharedData::OperationType::Scan)
            {
                // Walk past any additional queued scans to find the archive
                // op that consumes multi-root scan batches — each per-root
                // scan delivers its slice into the same ArchiveDownload.
                Operation* nextNonScan = nullptr;
                for (std::size_t lookahead = item.queueIndex + 1; lookahead < queue.size(); ++lookahead)
                {
                    auto* candidate = queue[lookahead].second.get();
                    if (candidate->type() != SharedData::OperationType::Scan)
                    {
                        nextNonScan = candidate;
                        break;
                    }
                }
                if (next && next->type() == SharedData::OperationType::BulkDownload)
                {
                    auto* scan = static_cast<ScanOperation*>(operation.get());
                    static_cast<BulkDownloadOperation*>(next)->setScanResult(scan->ejectEntries(), scan->totalBytes());
                }
                else if (next && next->type() == SharedData::OperationType::Delete)
                {
                    auto* scan = static_cast<ScanOperation*>(operation.get());
                    static_cast<DeleteOperation*>(next)->setScanResult(scan->ejectEntries(), scan->totalBytes());
                }
                else if (nextNonScan && nextNonScan->type() == SharedData::OperationType::ArchiveDownload)
                {
                    auto* scan = static_cast<ScanOperation*>(operation.get());
                    static_cast<ArchiveDownloadOperation*>(nextNonScan)
                        ->setScanResultForRoot(scan->remotePath(), scan->ejectEntries(), scan->totalBytes());
                }
                else
                {
                    const auto cbIt = syncScanCallbacks_.find(id.value());
                    if (cbIt != syncScanCallbacks_.end())
                    {
                        auto* scan = static_cast<ScanOperation*>(operation.get());
                        auto tree = scan->ejectScanTree();
                        auto callback = std::move(cbIt->second);
                        syncScanCallbacks_.erase(cbIt);
                        callback(std::move(tree));
                    }
                    else
                        Log::error("Scan operation completed but no following BulkOperation to set results to.");
                }
            }
            else if (operation->type() == SharedData::OperationType::LocalScan)
            {
                // Mirror the remote-Scan branch: walk past other LocalScans so
                // multi-root ArchiveUpload sits any number of slots ahead.
                Operation* nextNonScan = nullptr;
                for (std::size_t lookahead = item.queueIndex + 1; lookahead < queue.size(); ++lookahead)
                {
                    auto* candidate = queue[lookahead].second.get();
                    if (candidate->type() != SharedData::OperationType::LocalScan)
                    {
                        nextNonScan = candidate;
                        break;
                    }
                }
                if (next && next->type() == SharedData::OperationType::BulkUpload)
                {
                    auto* scan = static_cast<LocalScanOperation*>(operation.get());
                    static_cast<BulkUploadOperation*>(next)->setScanResult(scan->ejectEntries(), scan->totalBytes());
                }
                else if (nextNonScan && nextNonScan->type() == SharedData::OperationType::ArchiveUpload)
                {
                    auto* scan = static_cast<LocalScanOperation*>(operation.get());
                    static_cast<ArchiveUploadOperation*>(nextNonScan)
                        ->setScanResultForRoot(scan->localPath(), scan->ejectEntries(), scan->totalBytes());
                }
                else
                {
                    const auto cbIt = syncScanCallbacks_.find(id.value());
                    if (cbIt != syncScanCallbacks_.end())
                    {
                        auto* scan = static_cast<LocalScanOperation*>(operation.get());
                        auto tree = scan->ejectScanTree();
                        auto callback = std::move(cbIt->second);
                        syncScanCallbacks_.erase(cbIt);
                        callback(std::move(tree));
                    }
                    else
                        Log::error("LocalScan operation completed but no following BulkOperation to set results to.");
                }
            }

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
    std::filesystem::path const& localPath,
    std::filesystem::path const& remotePath,
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
                    self->hub_->callRemote(
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
        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::Download,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .totalBytes = result->size,
                .localPath = localPath,
                .remotePath = remotePath,
            }
        );

        return {};
    }
    else if (result->isDirectory())
    {
        // operationId is assigned to BulkDownload so the frontend's completion callback
        // (registered on operationId) fires only after all files are fully downloaded,
        // not when the preceding scan finishes.
        const auto scanId = Ids::generateOperationId();
        auto scan = std::make_unique<ScanOperation>(
            sftp,
            ScanOperation::ScanOperationOptions{
                .progressCallback = makeScanProgressCallback("onScanProgress", scanId),
                .remotePath = remotePath,
                .futureTimeout = std::chrono::seconds{5},
            }
        );

        auto bulk = std::make_unique<BulkDownloadOperation>(
            sftp,
            BulkDownloadOperation::BulkDownloadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkDownloadProgress", operationId),
                .remotePath = remotePath,
                .localPath = localPath,
                .individualOptions = resolveDownloadOptions(transferOptions, resolvedTimeout),
                .failFast = transferOptions.failFast.value_or(false),
            }
        );

        auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
        targetQueue.emplace_back(scanId, std::move(scan));
        targetQueue.emplace_back(operationId, std::move(bulk));

        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = scanId,
                .type = SharedData::OperationType::Scan,
                .mode = mode,
                .remotePath = remotePath,
            }
        );
        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::BulkDownload,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .localPath = localPath,
                .remotePath = remotePath,
            }
        );

        // Synthetic single-entry resume backup: re-issuing this through
        // adopt walks the same scan+bulk path, so the entry list itself
        // doesn't matter — we just need the directory paths preserved.
        SharedData::BulkAddRequest synthetic{
            .entries = {SharedData::BulkAddEntry{.src = remotePath, .dst = localPath, .isDirectory = true}},
            .allowOverwrite = allowOverwrite,
            .insertRefresh = insertRefresh,
            .mode = mode,
        };
        bulkResumeStash_.insert_or_assign(
            operationId, BulkResumeEntry{.kind = BulkResumeEntry::Kind::BulkDownload, .request = std::move(synthetic)}
        );

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
    std::vector<SharedData::DirectoryEntry> entries,
    std::filesystem::path const& localArchivePath,
    TarArchive::Compression compression,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode
)
{
    // Assumed in strand.

    if (entries.empty())
    {
        Log::error("addArchiveDownloadOperation: no entries provided.");
        return std::unexpected(
            Operation::Error{
                .type = Operation::ErrorType::InvalidPath,
                .extraInfo = "archive has no entries",
            }
        );
    }

    std::uint64_t totalPayloadBytes = 0u;
    for (auto const& entry : entries)
    {
        if (entry.type == SharedData::FileType::Regular)
            totalPayloadBytes += entry.size;
    }
    const auto rootCount = entries.size();

    // Collect each directory root's full remote path so we can enqueue a
    // preceding ScanOperation per root. The archive operation consumes those
    // scans' results via setScanResultForRoot and never recurses itself, so
    // scan work is always visible as a separate queue card.
    std::vector<std::filesystem::path> directoryRootPaths;
    for (auto const& entry : entries)
    {
        if (entry.type == SharedData::FileType::Directory)
        {
            const auto rootFullPath = entry.fullPath.empty() ? entry.path : entry.fullPath;
            directoryRootPaths.push_back(rootFullPath);
        }
    }

    ArchiveDownloadOperation::Options opts{};
    opts.entries = std::move(entries);
    opts.localArchivePath = localArchivePath;
    opts.compression = compression;
    opts.compressionOptions = compressionOptionsFromLevel(compression, compressionLevel);
    opts.mayOverwrite = mayOverwrite;
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
                self->hub_->callRemote(
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

    // Scans first so the dispatcher can hand each scan's results to the
    // archive op sitting one-or-more slots ahead.  They share a single queue
    // position (adjacency scan → ... → scan → archive), which the scan-
    // completion handler in workQueue walks past to reach the archive op.
    for (auto const& rootPath : directoryRootPaths)
    {
        const auto scanId = Ids::generateOperationId();
        auto scan = std::make_unique<ScanOperation>(
            sftp,
            ScanOperation::ScanOperationOptions{
                .progressCallback = makeScanProgressCallback("onScanProgress", scanId),
                .remotePath = rootPath,
                .futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout),
            }
        );
        targetQueue.emplace_back(scanId, std::move(scan));
        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = scanId,
                .type = SharedData::OperationType::Scan,
                .mode = mode,
                .remotePath = rootPath,
            }
        );
    }

    targetQueue.emplace_back(operationId, std::make_unique<ArchiveDownloadOperation>(sftp, std::move(opts)));

    Log::info(
        "OperationQueue::{}::onOperationAdded (ArchiveDownload, {} entries, {} bytes, {} pre-scans)",
        sessionId_.value(),
        rootCount,
        totalPayloadBytes,
        directoryRootPaths.size()
    );
    hub_->callRemote(
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::ArchiveDownload,
            .mode = mode,
            .insertRefresh = true,
            .totalBytes = totalPayloadBytes,
            .localPath = localArchivePath,
        }
    );

    return {};
}

std::expected<void, Operation::Error> OperationQueue::addArchiveUploadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::vector<std::filesystem::path> localPaths,
    std::filesystem::path const& remoteArchivePath,
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
        const auto size = std::filesystem::file_size(localPath, sizeErr);
        if (!sizeErr)
            estimatedTotalBytes += size;
    }

    // Local directory roots get a visible LocalScanOperation each; the
    // archive op itself never recurses and instead consumes each scan's
    // output via setScanResultForRoot — keyed by the root's local path.
    std::vector<std::filesystem::path> directoryRootPaths;
    for (auto const& localPath : localPaths)
    {
        std::error_code isDirErr{};
        if (std::filesystem::is_directory(localPath, isDirErr))
            directoryRootPaths.push_back(localPath);
    }
    const auto rootCount = localPaths.size();

    ArchiveUploadOperation::Options opts{};
    opts.localPaths = std::move(localPaths);
    opts.remoteArchivePath = remoteArchivePath;
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
                self->hub_->callRemote(
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

    // Scans first: the scan-complete dispatcher walks past subsequent scans
    // to reach this archive op and feeds each LocalScan's slice into its
    // setScanResultForRoot.  Queue layout: [LocalScan...] ... [ArchiveUpload].
    for (auto const& rootPath : directoryRootPaths)
    {
        const auto scanId = Ids::generateOperationId();
        auto scan = std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{
            .progressCallback = makeScanProgressCallback("onLocalScanProgress", scanId),
            .localPath = rootPath,
        });
        targetQueue.emplace_back(scanId, std::move(scan));
        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = scanId,
                .type = SharedData::OperationType::LocalScan,
                .mode = mode,
                .localPath = rootPath,
            }
        );
    }

    targetQueue.emplace_back(operationId, std::make_unique<ArchiveUploadOperation>(sftp, std::move(opts)));

    Log::info(
        "OperationQueue::{}::onOperationAdded (ArchiveUpload, {} paths, ~{} bytes, {} pre-scans)",
        sessionId_.value(),
        rootCount,
        estimatedTotalBytes,
        directoryRootPaths.size()
    );
    hub_->callRemote(
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::ArchiveUpload,
            .mode = mode,
            .insertRefresh = true,
            .totalBytes = estimatedTotalBytes,
            .remotePath = remoteArchivePath,
        }
    );
    return {};
}

std::size_t OperationQueue::addBulkDownloadOperation(
    SecureShell::SftpSession& sftp,
    SharedData::BulkAddRequest const& request,
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

    std::vector<BulkDownloadOperation::PrescannedFile> files;
    files.reserve(request.entries.size());
    std::size_t directoryCount = 0;
    for (std::size_t idx = 0; idx < request.entries.size(); ++idx)
    {
        auto const& entry = request.entries[idx];
        if (entry.isDirectory)
        {
            const auto result = addDownloadOperation(
                sftp,
                operationIdFor(idx),
                entry.dst,
                entry.src,
                request.allowOverwrite,
                /*isBigFile*/ false,
                /*insertRefresh*/ false,
                /*createMissingDirectories*/ true,
                request.mode
            );
            if (!result.has_value())
            {
                Log::error(
                    "Bulk download: directory entry '{}' failed to queue: {}",
                    entry.src.generic_string(),
                    result.error().toString()
                );
                continue;
            }
            ++directoryCount;
            continue;
        }
        files.push_back(
            BulkDownloadOperation::PrescannedFile{
                .remoteSrc = entry.src,
                .localDst = entry.dst,
                .sizeBytes = entry.sizeBytes,
                .mtime = entry.mtime,
                .mtimeNsec = entry.mtimeNsec,
            }
        );
    }

    if (!files.empty())
    {
        // Dedicated operationId for the aggregate bulk-download card, reserved
        // by the frontend separately from the per-entry ids to avoid collisions
        // (entry-0's id would clash when entry 0 is a directory). The first
        // file entry drives the UI's "localPath"/"remotePath" labels for the
        // card — arbitrary pick; per-file paths show up in progress events.
        const auto bulkOpId = bulkCardId;
        const auto firstSrc = files.front().remoteSrc;
        const auto firstDst = files.front().localDst;

        auto downloadOpts = resolveDownloadOptions(transferOptions, resolvedTimeout);

        auto bulk = std::make_unique<BulkDownloadOperation>(
            sftp,
            BulkDownloadOperation::BulkDownloadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkDownloadProgress", bulkOpId),
                .remotePath = firstSrc.parent_path(),
                .localPath = firstDst.parent_path(),
                .individualOptions = std::move(downloadOpts),
                .failFast = transferOptions.failFast.value_or(false),
            }
        );
        bulk->setPrescannedFileList(std::move(files));

        targetQueue.emplace_back(bulkOpId, std::move(bulk));
        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = bulkOpId,
                .type = SharedData::OperationType::BulkDownload,
                .mode = request.mode,
                .insertRefresh = request.insertRefresh,
                .localPath = firstDst.parent_path(),
                .remotePath = firstSrc.parent_path(),
            }
        );
    }

    Log::info("Bulk download: queued one bulk card (for files) + {} directory entries", directoryCount);

    // Stash the request keyed by the aggregate bulk card id so a teardown
    // can hand it to the resume registry.  Mixed file/directory entries
    // are preserved as-is — adopt re-issues the request through this same
    // function which performs the same file/directory partition.
    if (!request.entries.empty())
    {
        bulkResumeStash_.insert_or_assign(
            bulkCardId, BulkResumeEntry{.kind = BulkResumeEntry::Kind::BulkDownload, .request = request}
        );
    }

    return request.entries.size();
}

std::expected<void, Operation::Error> OperationQueue::addUploadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::filesystem::path const& localPath,
    std::filesystem::path const& remotePath,
    bool allowOverwrite,
    bool isBigFile,
    bool insertRefresh,
    bool createMissingDirectories,
    SharedData::OperationMode mode
)
{
    // Assumed in strand

    std::error_code ec;
    // lstat-style: do not follow links here so dangling symlinks can still be uploaded as
    // symlinks (UploadOperation::handleSymlink recreates the literal on the remote).
    const auto symStat = std::filesystem::symlink_status(localPath, ec);

    if (ec)
    {
        Log::error("Failed to stat local file '{}': {}", localPath.generic_string(), ec.message());
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FileStatFailed});
    }

    const bool isSymlink = std::filesystem::is_symlink(symStat);
    std::filesystem::file_status stat = symStat;
    if (!isSymlink)
    {
        stat = std::filesystem::status(localPath, ec);
        if (ec)
        {
            Log::error("Failed to stat local file '{}': {}", localPath.generic_string(), ec.message());
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
                    self->hub_->callRemote(
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

        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::Upload,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .localPath = localPath,
                .remotePath = remotePath,
            }
        );

        return {};
    }
    else if (stat.type() == std::filesystem::file_type::directory)
    {
        auto scan = std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{
            .progressCallback = makeScanProgressCallback("onLocalScanProgress", operationId),
            .localPath = localPath,
        });

        const auto bulkId = Ids::generateOperationId();
        auto bulk = std::make_unique<BulkUploadOperation>(
            sftp,
            BulkUploadOperation::BulkUploadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkUploadProgress", bulkId),
                .remotePath = remotePath,
                .localPath = localPath,
                .individualOptions = resolveUploadOptions(transferOptions, resolvedTimeout),
                .failFast = transferOptions.failFast.value_or(false),
            }
        );

        auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
        targetQueue.emplace_back(operationId, std::move(scan));
        targetQueue.emplace_back(bulkId, std::move(bulk));

        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::LocalScan,
                .mode = mode,
                .localPath = localPath,
            }
        );
        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = bulkId,
                .type = SharedData::OperationType::BulkUpload,
                .mode = mode,
                .insertRefresh = insertRefresh,
                .localPath = localPath,
                .remotePath = remotePath,
            }
        );

        // Synthetic single-entry resume — keyed by the bulk card's id
        // (bulkId here, since the BulkUpload onOperationAdded uses bulkId
        // as the frontend's card id).  Adopt walks the same scan+bulk
        // path on replay.
        SharedData::BulkAddRequest synthetic{
            .entries = {SharedData::BulkAddEntry{.src = localPath, .dst = remotePath, .isDirectory = true}},
            .allowOverwrite = allowOverwrite,
            .insertRefresh = insertRefresh,
            .mode = mode,
        };
        bulkResumeStash_.insert_or_assign(
            bulkId, BulkResumeEntry{.kind = BulkResumeEntry::Kind::BulkUpload, .request = std::move(synthetic)}
        );

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
    SharedData::BulkAddRequest const& request,
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

    std::vector<BulkUploadOperation::PrescannedFile> files;
    files.reserve(request.entries.size());
    std::size_t directoryCount = 0;
    for (std::size_t idx = 0; idx < request.entries.size(); ++idx)
    {
        auto const& entry = request.entries[idx];
        if (entry.isDirectory)
        {
            const auto result = addUploadOperation(
                sftp,
                operationIdFor(idx),
                entry.src,
                entry.dst,
                request.allowOverwrite,
                /*isBigFile*/ false,
                /*insertRefresh*/ false,
                /*createMissingDirectories*/ true,
                request.mode
            );
            if (!result.has_value())
            {
                Log::error(
                    "Bulk upload: directory entry '{}' failed to queue: {}",
                    entry.src.generic_string(),
                    result.error().toString()
                );
                continue;
            }
            ++directoryCount;
            continue;
        }
        files.push_back(
            BulkUploadOperation::PrescannedFile{
                .localSrc = entry.src,
                .remoteDst = entry.dst,
                .sizeBytes = entry.sizeBytes,
            }
        );
    }

    if (!files.empty())
    {
        // Separate reserved id — see addBulkDownloadOperation for rationale.
        const auto bulkOpId = bulkCardId;
        const auto firstSrc = files.front().localSrc;
        const auto firstDst = files.front().remoteDst;

        auto uploadOpts = resolveUploadOptions(transferOptions, resolvedTimeout);
        uploadOpts.createMissingDirectories = true;

        auto bulk = std::make_unique<BulkUploadOperation>(
            sftp,
            BulkUploadOperation::BulkUploadOperationOptions{
                .overallProgressCallback = makeBulkProgressCallback("onBulkUploadProgress", bulkOpId),
                .remotePath = firstDst.parent_path(),
                .localPath = firstSrc.parent_path(),
                .individualOptions = std::move(uploadOpts),
                .failFast = transferOptions.failFast.value_or(false),
            }
        );
        bulk->setPrescannedFileList(std::move(files));

        targetQueue.emplace_back(bulkOpId, std::move(bulk));
        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = bulkOpId,
                .type = SharedData::OperationType::BulkUpload,
                .mode = request.mode,
                .insertRefresh = request.insertRefresh,
                .localPath = firstSrc.parent_path(),
                .remotePath = firstDst.parent_path(),
            }
        );
    }

    Log::info("Bulk upload: queued one bulk card (for files) + {} directory entries", directoryCount);

    if (!request.entries.empty())
    {
        bulkResumeStash_.insert_or_assign(
            bulkCardId, BulkResumeEntry{.kind = BulkResumeEntry::Kind::BulkUpload, .request = request}
        );
    }

    return request.entries.size();
}

std::expected<void, Operation::Error> OperationQueue::addDeleteOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::filesystem::path const& remotePath,
    bool recursive,
    bool insertRefresh,
    SharedData::OperationMode mode
)
{
    // Assumed in strand
    auto& targetQueue = (mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;

    auto statFut = sftp.stat(remotePath);
    if (statFut.wait_for(sftpOpts_.operationTimeout.value_or(defaultFutureTimeout)) != std::future_status::ready)
    {
        Log::error("addDeleteOperation: stat timed out for '{}'", remotePath.string());
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FutureTimeout});
    }
    const auto statResult = statFut.get();
    const bool isDirectory = statResult.has_value() && statResult->isDirectoryLike();
    const auto isBulk = recursive && isDirectory;

    if (isBulk)
    {
        const auto scanId = Ids::generateOperationId();

        auto scan = std::make_unique<ScanOperation>(
            sftp,
            ScanOperation::ScanOperationOptions{
                .progressCallback = makeScanProgressCallback("onScanProgress", scanId),
                .remotePath = remotePath,
                .futureTimeout = std::chrono::seconds{5},
            }
        );

        targetQueue.emplace_back(scanId, std::move(scan));

        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = scanId,
                .type = SharedData::OperationType::Scan,
                .mode = mode,
                .remotePath = remotePath,
            }
        );
    }

    auto operation = std::make_unique<DeleteOperation>(
        sftp,
        DeleteOperation::DeleteOperationOptions{
            .filesRemovedProgress =
                [weak = weak_from_this(), operationId, name = rpcName("onDeleteProgress")](
                    auto const& path, std::uint64_t filesDeleted, std::uint64_t totalFiles
                )
            {
                auto self = weak.lock();
                if (!self)
                    return;
                self->hub_->callRemote(
                    name,
                    SharedData::BulkDeleteProgress{
                        .operationId = operationId,
                        .currentFile = path,
                        .filesDeleted = filesDeleted,
                        .totalFiles = totalFiles,
                    }
                );
            },
            .remotePath = remotePath,
            .futureTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout),
            .recursive = recursive && isDirectory,
        }
    );

    targetQueue.emplace_back(operationId, std::move(operation));

    hub_->callRemote(
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::Delete,
            .mode = mode,
            .insertRefresh = insertRefresh,
            .remotePath = remotePath,
        }
    );

    return {};
}

std::size_t OperationQueue::addBulkDeleteOperation(
    SecureShell::SftpSession& sftp,
    SharedData::BulkAddRequest const& request,
    Ids::OperationId bulkOperationId
)
{
    // Assumed in strand.  Strategy: build a single DeleteOperation with
    // pre-filled entries for all *file* paths in the request — that gives
    // one bulk-delete card on the frontend driven by the existing
    // BulkDeleteProgress emit path.  Each *directory* entry still needs the
    // recursive scan-then-delete flow so descendants are removed; those
    // become standard single-Delete cards via addDeleteOperation.

    auto& targetQueue = (request.mode == SharedData::OperationMode::PriorityQueued) ? priorityOperations_ : operations_;
    const auto resolvedTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);

    std::vector<SharedData::DirectoryEntry> fileEntries;
    fileEntries.reserve(request.entries.size());
    std::size_t enqueuedDirs = 0;

    for (auto const& entry : request.entries)
    {
        if (!entry.isDirectory)
        {
            SharedData::DirectoryEntry de{};
            de.path = entry.src;
            de.fullPath = entry.src;
            de.type = SharedData::FileType::Regular;
            de.size = entry.sizeBytes;
            fileEntries.push_back(std::move(de));
            continue;
        }
        // Directory entry → existing single-add path which sets up the
        // Scan + Delete pair.  These render as standard Scan / Delete
        // cards (separate from the bulk-files card below).
        const auto dirOpId = Ids::generateOperationId();
        const auto result = addDeleteOperation(
            sftp,
            dirOpId,
            entry.src,
            /*recursive*/ true,
            /*insertRefresh*/ false,
            request.mode
        );
        if (!result.has_value())
        {
            Log::error(
                "Bulk delete: directory entry '{}' failed to queue: {}",
                entry.src.generic_string(),
                result.error().toString()
            );
            continue;
        }
        ++enqueuedDirs;
    }

    if (!fileEntries.empty())
    {
        const auto fileCount = fileEntries.size();
        // recursive=true so DeleteOperation walks the prefilled entries_
        // (its non-recursive branch ignores entries_ and only deletes the
        // top-level remotePath).  remotePath is left empty; not used in
        // recursive mode.
        auto operation = std::make_unique<DeleteOperation>(
            sftp,
            DeleteOperation::DeleteOperationOptions{
                .filesRemovedProgress =
                    [weak = weak_from_this(), bulkOperationId, name = rpcName("onDeleteProgress")](
                        auto const& path, std::uint64_t filesDeleted, std::uint64_t totalFiles
                    )
                {
                    auto self = weak.lock();
                    if (!self)
                        return;
                    self->hub_->callRemote(
                        name,
                        SharedData::BulkDeleteProgress{
                            .operationId = bulkOperationId,
                            .currentFile = path,
                            .filesDeleted = filesDeleted,
                            .totalFiles = totalFiles,
                        }
                    );
                },
                .remotePath = {},
                .futureTimeout = resolvedTimeout,
                .recursive = true,
            }
        );
        operation->setScanResult(std::move(fileEntries), 0);
        targetQueue.emplace_back(bulkOperationId, std::move(operation));

        hub_->callRemote(
            rpcName("onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = bulkOperationId,
                .type = SharedData::OperationType::BulkDelete,
                .mode = request.mode,
                .insertRefresh = request.insertRefresh,
                .totalBytes = static_cast<std::uint64_t>(fileCount),
            }
        );
    }

    Log::info(
        "Bulk delete: {} file entries (one card) + {} directory entries",
        request.entries.size() - enqueuedDirs,
        enqueuedDirs
    );

    if (!request.entries.empty())
    {
        bulkResumeStash_.insert_or_assign(
            bulkOperationId, BulkResumeEntry{.kind = BulkResumeEntry::Kind::BulkDelete, .request = request}
        );
    }

    return request.entries.size();
}

std::expected<void, Operation::Error> OperationQueue::addRenameOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::filesystem::path const& sourcePath,
    std::filesystem::path const& destinationPath,
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

    hub_->callRemote(
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = operationId,
            .type = SharedData::OperationType::Rename,
            .mode = mode,
            .localPath = sourcePath,
            .remotePath = destinationPath,
        }
    );

    return {};
}

void OperationQueue::addSyncScanOperation(
    SecureShell::SftpSession& sftp,
    Ids::SyncSessionId syncSessionId,
    Ids::OperationId remoteScanId,
    Ids::OperationId localScanId,
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    bool respectIgnoreFiles,
    bool recursive,
    bool ignoreHidden
)
{
    // Create (or replace) the SyncSession.  A fresh id from the frontend is the
    // norm; re-opening over an existing id is handled as "discard previous state".
    auto session = std::make_shared<SyncSession>(
        SyncSession::Options{
            .sessionId = syncSessionId,
            .localRoot = localPath,
            .remoteRoot = remotePath,
        },
        executor_
    );
    syncSessions_[syncSessionId.value()] = session;

    // Scan completion callbacks hand the ScanNode trees into the session on its
    // own strand, then emit onSyncScanPhaseDone to the frontend.
    syncScanCallbacks_[remoteScanId.value()] =
        [weak = weak_from_this(), syncSessionId, weakSession = std::weak_ptr{session}](SharedData::Sync::ScanNode tree)
    {
        auto self = weak.lock();
        auto sess = weakSession.lock();
        if (!self || !sess)
            return;
        boost::asio::post(
            sess->strand(),
            [sess, tree = std::move(tree)]() mutable
            {
                sess->setRemoteTreeInStrand(std::move(tree));
            }
        );
        self->hub_->callRemote(self->rpcName("onSyncScanPhaseDone"), syncSessionId, /*isLocal=*/false);
    };

    syncScanCallbacks_[localScanId.value()] =
        [weak = weak_from_this(), syncSessionId, weakSession = std::weak_ptr{session}](SharedData::Sync::ScanNode tree)
    {
        auto self = weak.lock();
        auto sess = weakSession.lock();
        if (!self || !sess)
            return;
        boost::asio::post(
            sess->strand(),
            [sess, tree = std::move(tree)]() mutable
            {
                sess->setLocalTreeInStrand(std::move(tree));
            }
        );
        self->hub_->callRemote(self->rpcName("onSyncScanPhaseDone"), syncSessionId, /*isLocal=*/true);
    };

    // Queue remote scan — build-tree mode.
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
        }
    );
    priorityOperations_.emplace_back(remoteScanId, std::move(remoteScan));

    hub_->callRemote(
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
    });
    priorityOperations_.emplace_back(localScanId, std::move(localScan));

    hub_->callRemote(
        rpcName("onOperationAdded"),
        SharedData::OperationAdded{
            .operationId = localScanId,
            .type = SharedData::OperationType::LocalScan,
            .mode = SharedData::OperationMode::PriorityQueued,
            .localPath = localPath,
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
