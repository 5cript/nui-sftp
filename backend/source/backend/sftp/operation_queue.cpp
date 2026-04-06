#include <backend/sftp/operation_queue.hpp>
#include <shared_data/file_operations/transfer_progress.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/is_paused.hpp>

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

    DownloadOperation::DownloadOperationOptions resolveDownloadOptions(
        Persistence::DownloadOptions const& opts,
        std::chrono::seconds futureTimeout
    )
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

    UploadOperation::UploadOperationOptions resolveUploadOptions(
        Persistence::UploadOptions const& opts,
        std::chrono::seconds futureTimeout
    )
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
    return [weak = weak_from_this(), name = rpcName(eventName), operationId](
               std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)
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
               std::make_signed_t<std::size_t> bytesPerSecond)
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
                    op.second->cancel(true);
                return isMatch;
            };

            const auto erasedFromRegular =
                std::erase_if(self->operations_, cancelPredicate);
            if (erasedFromRegular == 0)
                std::erase_if(self->priorityOperations_, cancelPredicate);
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

            // Log::info(
            //     "Operation completed: id={}, reason={}, localPath='{}', remotePath='{}'",
            //     operationCompleted.operationId.value(),
            //     static_cast<int>(operationCompleted.reason),
            //     operationCompleted.localPath ? operationCompleted.localPath->generic_string() : "<none>",
            //     operationCompleted.remotePath ? operationCompleted.remotePath->generic_string() : "<none>"
            // );

            self->hub_->callRemote(
                self->rpcName("onOperationCompleted"),
                std::move(operationCompleted)
            );
        }
    );
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
    // Assumed in strand

    const auto updateCount = std::min(queue.size(), static_cast<std::size_t>(parallelism_));

    if (updateCount == 0)
        return false;

    bool moreWork = false;
    bool previousWasBarrier = false;
    for (std::size_t i = 0; i < updateCount; ++i)
    {
        if (previousWasBarrier)
            break;

        auto& [id, operation] = queue[i];
        previousWasBarrier = operation->isBarrier();

        const auto workResult = operation->work();
        if (!workResult.has_value())
        {
            completeOperation(
                makeCompletedOperation(OperationQueue::CompletionReason::Failed, id, *operation, workResult.error())
            );
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }

        const auto workStatus = workResult.value();
        if (workStatus == Operation::WorkStatus::Complete)
        {
            auto* next = (i + 1 < queue.size()) ? queue[i + 1].second.get() : nullptr;
            if (operation->type() == SharedData::OperationType::Scan)
            {
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
                else
                    Log::error("Scan operation completed but no following BulkOperation to set results to.");
            }
            else if (operation->type() == SharedData::OperationType::LocalScan)
            {
                if (next && next->type() == SharedData::OperationType::BulkUpload)
                {
                    auto* scan = static_cast<LocalScanOperation*>(operation.get());
                    static_cast<BulkUploadOperation*>(next)->setScanResult(scan->ejectEntries(), scan->totalBytes());
                }
                else
                    Log::error("Scan operation completed but no following BulkOperation to set results to.");
            }

            completeOperation(makeCompletedOperation(OperationQueue::CompletionReason::Completed, id, *operation));
            queue.pop_front();
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
        opts.progressCallback =
            [weak = weak_from_this(), operationId, name = rpcName("onDownloadProgress")](
                auto min, auto max, auto current, auto bytesPerSecond)
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
        auto scan = std::make_unique<ScanOperation>(
            sftp,
            ScanOperation::ScanOperationOptions{
                .progressCallback = makeScanProgressCallback("onScanProgress", operationId),
                .remotePath = remotePath,
                .futureTimeout = std::chrono::seconds{5},
            }
        );

        // operationId is assigned to BulkDownload so the frontend's completion callback
        // (registered on operationId) fires only after all files are fully downloaded,
        // not when the preceding scan finishes.
        const auto scanId = Ids::generateOperationId();
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

        return {};
    }
    else
    {
        Log::error("Remote path is neither a file nor a directory: {}.", static_cast<std::uint8_t>(result->type));
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::OperationNotPossibleOnFileType});
    }
}

std::expected<void, Operation::Error> OperationQueue::addUploadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::filesystem::path const& localPath,
    std::filesystem::path const& remotePath,
    bool allowOverwrite,
    bool isBigFile,
    bool insertRefresh,
    SharedData::OperationMode mode
)
{
    // Assumed in strand

    std::error_code ec;
    const auto stat = std::filesystem::status(localPath, ec);

    if (ec)
    {
        Log::error("Failed to stat local file '{}': {}", localPath.generic_string(), ec.message());
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FileStatFailed});
    }

    auto transferOptions = sftpOpts_.uploadOptions.value_or(Persistence::UploadOptions{});
    if (allowOverwrite)
        transferOptions.mayOverwrite = allowOverwrite;

    const auto resolvedTimeout = sftpOpts_.operationTimeout.value_or(defaultFutureTimeout);

    if (stat.type() == std::filesystem::file_type::regular)
    {
        auto opts = resolveUploadOptions(transferOptions, resolvedTimeout);
        opts.remotePath = remotePath;
        opts.localPath = localPath;
        opts.bigFileOptimized = isBigFile;
        opts.progressCallback =
            [weak = weak_from_this(), operationId, name = rpcName("onUploadProgress")](
                auto min, auto max, auto current, auto bytesPerSecond)
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

        return {};
    }
    else
    {
        Log::error("Local path is neither a file nor a directory: {}.", static_cast<std::uint8_t>(stat.type()));
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::OperationNotPossibleOnFileType});
    }
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

    if (recursive && isDirectory)
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
                    auto const& path, std::uint64_t filesDeleted, std::uint64_t totalFiles)
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
}
