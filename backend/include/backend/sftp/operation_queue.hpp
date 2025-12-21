#pragma once

#include <backend/sftp/all_operations.hpp>
#include <persistence/state/state.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/rpc.hpp>
#include <ids/ids.hpp>
#include <backend/rpc_helper.hpp>
#include <shared_data/file_operations/operation_completed.hpp>

#include <deque>
#include <filesystem>
#include <memory>
#include <utility>
#include <atomic>

class OperationQueue
    : public RpcHelper::StrandRpc
    , public std::enable_shared_from_this<OperationQueue>
{
  public:
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
        int parallelism = 1);

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
        std::filesystem::path const& remotePath);

    std::expected<void, Operation::Error> addUploadOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& localPath,
        std::filesystem::path const& remotePath);

    void registerRpc();

    bool paused() const;
    void paused(bool pause);

  private:
    void completeOperation(OperationCompleted&& operationCompleted);

  private:
    Persistence::SftpOptions sftpOpts_{};
    Ids::SessionId sessionId_{};
    std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>> operations_{};
    std::atomic_bool paused_{true};
    int parallelism_{1};
};
