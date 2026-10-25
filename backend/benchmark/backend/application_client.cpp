#include "application_client.hpp"

#include <backend/sftp/bulk_download_operation.hpp>
#include <backend/sftp/bulk_upload_operation.hpp>
#include <backend/sftp/download_operation.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <constants/sftp.hpp>
#include <paths/local_path.hpp>
#include <paths/native.hpp>
#include <paths/remote_path.hpp>

#include <fmt/format.h>

#include <chrono>
#include <future>
#include <memory>

namespace Benchmark
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr auto transferTimeout = std::chrono::seconds{600};

        std::expected<Paths::RemotePath, std::string> remotePathOf(std::string const& path)
        {
            return Paths::RemotePath::make(path).transform_error(
                [&path](auto const&)
                {
                    return fmt::format("invalid remote path {}", path);
                }
            );
        }

        std::expected<Paths::LocalPath, std::string> localPathOf(std::filesystem::path const& path)
        {
            return Paths::LocalPath::make(path).transform_error(
                [&path](auto const&)
                {
                    return fmt::format("invalid local path {}", Paths::nativeToUtf8(path));
                }
            );
        }
    }

    ApplicationClient::ApplicationClient(Connection& connection)
        : connection_{connection}
    {}

    std::string ApplicationClient::name() const
    {
        return "nui-sftp";
    }

    std::expected<void, std::string> ApplicationClient::transfer(Transfer const& transfer)
    {
        return transfer.direction == Direction::Download ? download(transfer) : upload(transfer);
    }

    std::expected<void, std::string> ApplicationClient::download(Transfer const& transfer)
    {
        const auto remote = remotePathOf(transfer.remotePath);
        const auto local = localPathOf(transfer.localPath);
        if (!remote)
            return std::unexpected{remote.error()};
        if (!local)
            return std::unexpected{local.error()};

        if (transfer.workload->isSingleFile())
        {
            // OperationQueue::addDownloadOperation stats the file first, its size picks the transfer path.
            auto statFuture = connection_.sftp->lstat(*remote);
            if (statFuture.wait_for(10s) != std::future_status::ready)
                return std::unexpected<std::string>{"lstat timed out"};
            const auto information = statFuture.get();
            if (!information)
                return std::unexpected{fmt::format("lstat failed: {}", information.error().message)};

            DownloadOperation operation{
                connection_.sftp,
                {
                    .remotePath = *remote,
                    .localPath = *local,
                    .bigFileOptimized = information->size >= Constants::bigFileCutOff,
                    .entry = *information,
                }
            };
            return runToCompletion(operation, transferTimeout);
        }

        BulkDownloadOperation operation{
            connection_.sftp,
            {
                .remotePath = *remote,
                .localPath = *local,
                .concurrency = bulkConcurrency(),
            }
        };
        operation.scanFirst(std::make_unique<ScanOperation>(
            connection_.sftp, ScanOperation::ScanOperationOptions{.remotePath = *remote}
        ));
        return runToCompletion(operation, transferTimeout);
    }

    std::expected<void, std::string> ApplicationClient::upload(Transfer const& transfer)
    {
        const auto remote = remotePathOf(transfer.remotePath);
        const auto local = localPathOf(transfer.localPath);
        if (!remote)
            return std::unexpected{remote.error()};
        if (!local)
            return std::unexpected{local.error()};

        if (transfer.workload->isSingleFile())
        {
            UploadOperation operation{
                connection_.sftp,
                {
                    .remotePath = *remote,
                    .localPath = *local,
                    .bigFileOptimized = transfer.workload->fileSize >= Constants::bigFileCutOff,
                }
            };
            return runToCompletion(operation, transferTimeout);
        }

        BulkUploadOperation operation{
            connection_.sftp,
            {
                .remotePath = *remote,
                .localPath = *local,
                .concurrency = bulkConcurrency(),
            }
        };
        operation.scanFirst(std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{
            .localPath = *local,
        }));
        return runToCompletion(operation, transferTimeout);
    }
}
