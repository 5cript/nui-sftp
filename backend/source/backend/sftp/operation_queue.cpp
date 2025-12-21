#include <backend/sftp/operation_queue.hpp>
#include <shared_data/file_operations/download_progress.hpp>
#include <shared_data/file_operations/upload_progress.hpp>
#include <shared_data/file_operations/bulk_download_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/is_paused.hpp>

#include <log/log.hpp>
#include <utility/overloaded.hpp>

namespace
{
    OperationQueue::OperationCompleted makeCompletedOperation(
        OperationQueue::CompletionReason reason,
        Ids::OperationId operationId,
        Operation const& operation,
        std::optional<Operation::Error> error = std::nullopt)
    {
        return operation.visit(
            Utility::overloaded(
                [reason, operationId, error](DownloadOperation const& op) {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .localPath = op.localPath(),
                        .remotePath = op.remotePath(),
                        .error = error,
                    };
                },
                [reason, operationId, error](ScanOperation const& op) {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .remotePath = op.remotePath(),
                        .error = error,
                    };
                },
                [reason, operationId, error](BulkDownloadOperation const&) {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                        .error = error,
                    };
                },
                [reason, operationId, error](UploadOperation const&) {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                    };
                },
                [reason, operationId](std::nullopt_t) {
                    return OperationQueue::OperationCompleted{
                        .reason = reason,
                        .operationId = operationId,
                        .completionTime = std::chrono::system_clock::now(),
                    };
                }));
    }
}

OperationQueue::OperationQueue(
    boost::asio::any_io_executor executor,
    std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    Persistence::SftpOptions sftpOpts,
    Ids::SessionId sessionId,
    int parallelism)
    : RpcHelper::StrandRpc{executor, strand, wnd, hub}
    , sftpOpts_{std::move(sftpOpts)}
    , sessionId_{std::move(sessionId)}
    , parallelism_{parallelism}
{}

void OperationQueue::cancelAll()
{
    within_strand_do([weak = weak_from_this()]() {
        auto self = weak.lock();
        if (!self)
            return;

        self->operations_.clear();
        Log::info("All operations in the queue have been canceled.");
    });
}
void OperationQueue::cancel(Ids::OperationId id)
{
    within_strand_do([weak = weak_from_this(), id = std::move(id)]() {
        auto self = weak.lock();
        if (!self)
            return;

        std::erase_if(self->operations_, [id](auto& op) {
            bool isMatch = op.first == id;
            if (isMatch)
                op.second->cancel(true);
            return isMatch;
        });
    });
}

void OperationQueue::completeOperation(SharedData::OperationCompleted&& operationCompleted)
{
    within_strand_do([weak = weak_from_this(), operationCompleted = std::move(operationCompleted)]() {
        auto self = weak.lock();
        if (!self)
            return;

        if (operationCompleted.error)
            Log::error("Operation failed: {}", operationCompleted.error->toString());

        Log::info(
            "Operation completed: id={}, reason={}, localPath='{}', remotePath='{}'",
            operationCompleted.operationId.value(),
            static_cast<int>(operationCompleted.reason),
            operationCompleted.localPath ? operationCompleted.localPath->generic_string() : "<none>",
            operationCompleted.remotePath ? operationCompleted.remotePath->generic_string() : "<none>");

        self->hub_->callRemote(
            fmt::format("OperationQueue::{}::onOperationCompleted", self->sessionId_.value()),
            SharedData::OperationCompleted{
                .reason = operationCompleted.reason,
                .operationId = operationCompleted.operationId,
                .completionTime = operationCompleted.completionTime,
                .localPath = operationCompleted.localPath,
                .remotePath = operationCompleted.remotePath,
                .error = operationCompleted.error,
            });
    });
}

bool OperationQueue::work()
{
    // Assumed in strand

    if (paused_)
        return false;

    const auto updateCount = std::min(operations_.size(), static_cast<std::size_t>(parallelism_));

    bool moreWork = false;
    if (updateCount == 0)
        return false;

    bool previousWasBarrier = false;
    for (std::size_t i = 0; i < updateCount; ++i)
    {
        if (previousWasBarrier)
            break;

        auto& [id, operation] = operations_[i];
        previousWasBarrier = operation->isBarrier();

        const auto workResult = operation->work();
        if (!workResult.has_value())
        {
            completeOperation(
                makeCompletedOperation(OperationQueue::CompletionReason::Failed, id, *operation, workResult.error()));
            operations_.erase(operations_.begin() + static_cast<std::ptrdiff_t>(i));
            // Exit loop and avoid any offset math. Just do another update cycle.
            return true;
        }

        const auto workStatus = workResult.value();
        if (workStatus == Operation::WorkStatus::Complete)
        {
            Log::info("Operation completed successfully: {}", id.value());
            if (operation->type() == SharedData::OperationType::Scan)
            {
                auto* next = (i + 1 < operations_.size()) ? operations_[i + 1].second.get() : nullptr;
                if (next && next->type() == SharedData::OperationType::BulkDownload)
                {
                    auto* scan = static_cast<ScanOperation*>(operation.get());
                    static_cast<BulkDownloadOperation*>(next)->setScanResult(scan->ejectEntries(), scan->totalBytes());
                }
                else
                {
                    Log::error("Scan operation completed but no following BulkOperation to set results to.");
                }
            }

            completeOperation(makeCompletedOperation(OperationQueue::CompletionReason::Completed, id, *operation));
            operations_.pop_front();
            // Exit loop and avoid any offset math. Just do another update cycle.
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

bool OperationQueue::paused() const
{
    return paused_;
}
void OperationQueue::paused(bool pause)
{
    within_strand_do([weak = weak_from_this(), pause]() {
        auto self = weak.lock();
        if (!self)
            return;

        self->paused_ = pause;
    });
}

std::expected<void, Operation::Error> OperationQueue::addDownloadOperation(
    SecureShell::SftpSession& sftp,
    Ids::OperationId operationId,
    std::filesystem::path const& localPath,
    std::filesystem::path const& remotePath)
{
    // Assumed in strand

    auto fut = sftp.stat(remotePath);
    if (fut.wait_for(sftpOpts_.operationTimeout) != std::future_status::ready)
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

    const auto transferOptions = sftpOpts_.downloadOptions.value_or(Persistence::DownloadOptions{});
    const auto defaultOptions = DownloadOperation::DownloadOperationOptions{};

    if (result->isRegularFile())
    {
        const auto fileSize = result->size;

        auto fut = sftp.openFile(remotePath, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::unknown);
        if (fut.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
        {
            Log::error("Failed to open remote sftp file: timeout");
            return std::unexpected(Operation::Error{.type = Operation::ErrorType::OpenFailure});
        }

        const auto openResult = fut.get();
        if (!openResult.has_value())
        {
            Log::error("Failed to open remote sftp file: {}", openResult.error().message);
            return std::unexpected(
                Operation::Error{.type = Operation::ErrorType::SftpError, .sftpError = openResult.error()});
        }

        auto operation = std::make_unique<DownloadOperation>(
            std::move(openResult).value(),
            DownloadOperation::DownloadOperationOptions{
                .progressCallback =
                    [weak = weak_from_this(), operationId](auto min, auto max, auto current) {
                        auto self = weak.lock();
                        if (!self)
                            return;

                        self->hub_->callRemote(
                            fmt::format("OperationQueue::{}::onDownloadProgress", self->sessionId_.value()),
                            SharedData::DownloadProgress{
                                .operationId = operationId,
                                .min = min,
                                .max = max,
                                .current = current,
                            });

                        Log::debug(
                            "Downloaded {} / {} bytes ({}%)",
                            current - min,
                            max - min,
                            (current - min) * 100 / (max - min));
                    },
                .remotePath = remotePath,
                .localPath = localPath,
                .tempFileSuffix = transferOptions.tempFileSuffix.value_or(defaultOptions.tempFileSuffix),
                .mayOverwrite = transferOptions.mayOverwrite.value_or(defaultOptions.mayOverwrite),
                .reserveSpace = transferOptions.reserveSpace.value_or(defaultOptions.reserveSpace),
                .tryContinue = transferOptions.tryContinue.value_or(defaultOptions.tryContinue),
                .inheritPermissions = transferOptions.inheritPermissions.value_or(defaultOptions.inheritPermissions),
                .doCleanup = transferOptions.doCleanup.value_or(defaultOptions.doCleanup),
                .permissions =
                    transferOptions.customPermissions ? transferOptions.customPermissions : defaultOptions.permissions,
            });

        operations_.emplace_back(operationId, std::move(operation));

        Log::info("Calling OperationQueue::{}::onOperationAdded", sessionId_.value());
        hub_->callRemote(
            fmt::format("OperationQueue::{}::onOperationAdded", sessionId_.value()),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::Download,
                .totalBytes = fileSize,
                .localPath = localPath,
                .remotePath = remotePath});

        return {};
    }
    else if (result->isDirectory())
    {
        auto scan = std::make_unique<ScanOperation>(
            sftp,
            ScanOperation::ScanOperationOptions{
                .progressCallback =
                    [weak = weak_from_this(), operationId](auto totalBytes, auto currentIndex, auto totalScanned) {
                        auto self = weak.lock();
                        if (!self)
                            return;

                        self->hub_->callRemote(
                            fmt::format("OperationQueue::{}::onScanProgress", self->sessionId_.value()),
                            SharedData::ScanProgress{
                                .operationId = operationId,
                                .totalBytes = totalBytes,
                                .currentIndex = currentIndex,
                                .totalScanned = totalScanned,
                            });
                    },
                .remotePath = remotePath,
                .futureTimeout = std::chrono::seconds{5},
            });

        // Cant use same ID for scan and bulk download
        const auto bulkId = Ids::generateOperationId();
        auto bulk = std::make_unique<BulkDownloadOperation>(
            sftp,
            BulkDownloadOperation::BulkDownloadOperationOptions{
                .overallProgressCallback =
                    [weak = weak_from_this(), bulkId](
                        auto const& currentFile,
                        std::uint64_t fileCurrentIndex,
                        std::uint64_t fileCount,
                        std::uint64_t currentFileBytes,
                        std::uint64_t currentFileTotalBytes,
                        std::uint64_t bytesCurrent,
                        std::uint64_t bytesTotal) {
                        auto self = weak.lock();
                        if (!self)
                            return;

                        // Log::debug(
                        //     "BulkDownloadOperation: Download progress for file: {} - {}/{} bytes - totaling {}/{} "
                        //     "bytes",
                        //     currentFile.string(),
                        //     currentFileBytes,
                        //     currentFileTotalBytes,
                        //     bytesCurrent,
                        //     bytesTotal);

                        self->hub_->callRemote(
                            fmt::format("OperationQueue::{}::onBulkDownloadProgress", self->sessionId_.value()),
                            SharedData::BulkDownloadProgress{
                                .operationId = bulkId,
                                .currentFile = currentFile.string(),
                                .fileCurrentIndex = fileCurrentIndex,
                                .fileCount = fileCount,
                                .currentFileBytes = currentFileBytes,
                                .currentFileTotalBytes = currentFileTotalBytes,
                                .bytesCurrent = bytesCurrent,
                                .bytesTotal = bytesTotal,
                            });
                    },
                .remotePath = remotePath,
                .localPath = localPath,
                .individualOptions =
                    DownloadOperation::DownloadOperationOptions{
                        .tempFileSuffix = transferOptions.tempFileSuffix.value_or(defaultOptions.tempFileSuffix),
                        .mayOverwrite = transferOptions.mayOverwrite.value_or(defaultOptions.mayOverwrite),
                        .reserveSpace = transferOptions.reserveSpace.value_or(defaultOptions.reserveSpace),
                        .tryContinue = transferOptions.tryContinue.value_or(defaultOptions.tryContinue),
                        .inheritPermissions =
                            transferOptions.inheritPermissions.value_or(defaultOptions.inheritPermissions),
                        .doCleanup = transferOptions.doCleanup.value_or(defaultOptions.doCleanup),
                        .permissions = transferOptions.customPermissions ? transferOptions.customPermissions
                                                                         : defaultOptions.permissions,
                    },
            });

        operations_.emplace_back(operationId, std::move(scan));
        operations_.emplace_back(bulkId, std::move(bulk));

        hub_->callRemote(
            fmt::format("OperationQueue::{}::{}", sessionId_.value(), "onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::Scan,
                .remotePath = remotePath,
            });
        hub_->callRemote(
            fmt::format("OperationQueue::{}::{}", sessionId_.value(), "onOperationAdded"),
            SharedData::OperationAdded{
                .operationId = bulkId,
                .type = SharedData::OperationType::BulkDownload,
                .localPath = localPath,
                .remotePath = remotePath,
            });

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
    std::filesystem::path const& remotePath)
{
    // Assumed in strand

    std::error_code ec;
    const auto stat = std::filesystem::status(localPath, ec);

    if (ec)
    {
        Log::error("Failed to stat local file '{}': {}", localPath.generic_string(), ec.message());
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FileStatFailed});
    }

    const auto transferOptions = sftpOpts_.uploadOptions.value_or(Persistence::UploadOptions{});
    const auto defaultOptions = UploadOperation::UploadOperationOptions{};

    if (stat.type() == std::filesystem::file_type::regular)
    {
        auto operation = std::make_unique<UploadOperation>(
            sftp,
            UploadOperation::UploadOperationOptions{
                .progressCallback =
                    [weak = weak_from_this(), operationId](auto min, auto max, auto current) {
                        auto self = weak.lock();
                        if (!self)
                            return;

                        self->hub_->callRemote(
                            fmt::format("OperationQueue::{}::onUploadProgress", self->sessionId_.value()),
                            SharedData::UploadProgress{
                                .operationId = operationId,
                                .min = min,
                                .max = max,
                                .current = current,
                            });

                        Log::debug(
                            "Uploaded {} / {} bytes ({}%)",
                            current - min,
                            max - min,
                            (current - min) * 100 / (max - min));
                    },
                .remotePath = remotePath,
                .localPath = localPath,
                .tempFileSuffix = transferOptions.tempFileSuffix.value_or(defaultOptions.tempFileSuffix),
                .mayOverwrite = transferOptions.mayOverwrite.value_or(defaultOptions.mayOverwrite),
                .tryContinue = transferOptions.tryContinue.value_or(defaultOptions.tryContinue),
                .inheritPermissions = transferOptions.inheritPermissions.value_or(defaultOptions.inheritPermissions),
                .permissions =
                    transferOptions.customPermissions ? transferOptions.customPermissions : defaultOptions.permissions,
                .futureTimeout = sftpOpts_.operationTimeout,
            });

        operations_.emplace_back(operationId, std::move(operation));

        hub_->callRemote(
            fmt::format("OperationQueue::{}::onOperationAdded", sessionId_.value()),
            SharedData::OperationAdded{
                .operationId = operationId,
                .type = SharedData::OperationType::Upload,
                .localPath = localPath,
                .remotePath = remotePath});

        return {};
    }
    else if (stat.type() == std::filesystem::file_type::directory)
    {
        // TODO: Bulk upload!
        Log::error("Bulk upload is not implemented yet.");
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::NotImplemented});
    }
    else
    {
        Log::error("Local path is neither a file nor a directory: {}.", static_cast<std::uint8_t>(stat.type()));
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::OperationNotPossibleOnFileType});
    }
}

void OperationQueue::registerRpc()
{
    on(fmt::format("OperationQueue::{}::isPaused", sessionId_.value()))
        .perform([weak = weak_from_this()](RpcHelper::RpcOnce&& reply) {
            auto self = weak.lock();
            if (!self)
                return reply(SharedData::error("OperationQueue no longer exists"));

            return reply(
                SharedData::ErrorOrSuccess{SharedData::IsPaused{
                    .paused = self->paused(),
                }});
        });

    on(fmt::format("OperationQueue::{}::cancel", sessionId_.value()))
        .perform([weak = weak_from_this()](RpcHelper::RpcOnce&& reply, Ids::OperationId operationId) {
            auto self = weak.lock();
            if (!self)
                return reply(SharedData::error("OperationQueue no longer exists"));

            self->cancel(std::move(operationId));
            return reply(SharedData::success());
        });
}