#pragma once

#include <backend/sftp/all_operations.hpp>
#include <persistence/state/state.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/rpc.hpp>
#include <ids/ids.hpp>
#include <backend/rpc_helper.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operation_mode.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <atomic>

class OperationQueue
    : public RpcHelper::StrandRpc
    , public std::enable_shared_from_this<OperationQueue>
{
  public:
    constexpr static std::chrono::seconds defaultFutureTimeout{5};

    using Error = SharedData::OperationErrorType;
    using OperationCompleted = SharedData::OperationCompleted;
    using CompletionReason = SharedData::OperationCompletionReason;

  public:
    OperationQueue(
        boost::asio::any_io_executor executor,
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::SftpOptions sftpOpts,
        Ids::SessionId sessionId,
        int parallelism = 1
    );

    void cancelAll();
    void cancel(Ids::OperationId id);

    /**
     * @brief Returns true if it should be called without delay again.
     *
     * @return true
     * @return false
     */
    bool work();

    std::expected<void, Operation::Error> addDownloadOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& localPath,
        std::filesystem::path const& remotePath,
        bool allowOverwrite,
        bool isBigFile,
        bool insertRefresh,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    std::expected<void, Operation::Error> addUploadOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& localPath,
        std::filesystem::path const& remotePath,
        bool allowOverwrite,
        bool isBigFile,
        bool insertRefresh,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    std::expected<void, Operation::Error> addDeleteOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& remotePath,
        bool recursive,
        bool insertRefresh,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    void registerRpc();

    bool paused() const;
    void paused(bool pause);

  private:
    void completeOperation(OperationCompleted&& operationCompleted);
    void deepPause(bool pause);

    std::string rpcName(std::string_view event) const;

    auto makeScanProgressCallback(std::string_view eventName, Ids::OperationId operationId)
        -> std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>;

    auto makeBulkProgressCallback(std::string_view eventName, Ids::OperationId operationId)
        -> std::function<void(
            std::filesystem::path const&,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::make_signed_t<std::size_t>
        )>;

  private:
    bool workQueue(std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>>& queue);

  private:
    Persistence::SftpOptions sftpOpts_{};
    Ids::SessionId sessionId_{};
    std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>> priorityOperations_{};
    std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>> operations_{};
    std::atomic_bool paused_{true};
    int parallelism_{1};
};
