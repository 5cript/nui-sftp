#pragma once

#include <ssh/session.hpp>
#include <ssh/sftp_session.hpp>
#include <ids/ids.hpp>
#include <backend/sftp/operation_queue.hpp>
#include <backend/rpc_helper.hpp>
#include <persistence/state_holder.hpp>

#include <unordered_map>
#include <memory>
#include <atomic>
#include <optional>

/**
 * @brief This session is the implementation equivalent of one tab in the UI.
 * A tab in the UI represents everything related to one server address and port.
 *
 * These sessions are managed by SessionManager. Each session has an id, as well as ssh channels, sftp channels and
 */
class Session
    : public RpcHelper::StrandRpc
    , public std::enable_shared_from_this<Session>
{
  public:
    constexpr static auto futureTimeout = std::chrono::seconds{10};
    constexpr static auto queueStartThrottle = std::chrono::milliseconds{5};
    constexpr static auto queueMaxThrottle = std::chrono::seconds{3};

    Session(
        Ids::SessionId id,
        std::unique_ptr<SecureShell::Session> session,
        boost::asio::any_io_executor executor,
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::SftpOptions const& sftpOptions);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() = default;

    void start();
    void stop();

  private:
    /**
     * Handles calls from the frontend to create a new channel with the following payload:
     * {
     *     engine: {
     *         sshSessionOptions: Persistence::SshSessionOptions,
     *         environment: std::unordered_map<std::string, std::string>
     *     },
     *     fileMode: boolean (optional, default false)
     * }
     *
     */
    void registerRpcCreateChannel();

    /**
     * Handles calls from the frontend to start reading from a channel with the following payload:
     * {
     *     channelId: string
     * }
     */
    void registerRpcStartChannelRead();

    /**
     * Handles calls from the frontend to close a channel with the following payload:
     * {
     *     channelId: string
     * }
     */
    void registerRpcChannelClose();

    /**
     * Handles calls from the frontend to write data to a channel with the following payload:
     * {
     *     channelId: string,
     *     data: string (base64 encoded)
     * }
     */
    void registerRpcChannelWrite();

    /**
     * Handles calls from the frontend to resize the pty of a channel with the following payload:
     * {
     *     channelId: string,
     *     cols: int,
     *     rows: int
     * }
     */
    void registerRpcChannelPtyResize();

    /**
     * Handles calls from the frontend to list a directory over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpListDirectory();

    /**
     * Handles calls from the frontend to create a directory over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpCreateDirectory();

    /**
     * Handles calls from the frontend to create a file over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpCreateFile();
    void registerRpcSftpAddDownloadOperation();
    void registerRpcSftpAddUploadOperation();
    void registerOperationQueuePauseUnpause();

    void removeChannel(Ids::ChannelId channelId);

    void removeSftpChannel(Ids::ChannelId channelId);

    template <typename FunctionT, typename ReplyCallable>
    void withChannelDo(Ids::ChannelId channelId, FunctionT&& func, ReplyCallable&& reply)
    {
        within_strand_do([this,
                          channelId = std::move(channelId),
                          func = std::forward<FunctionT>(func),
                          reply = std::forward<ReplyCallable>(reply)]() mutable {
            if (auto iter = channels_.find(channelId); iter != channels_.end())
            {
                if (auto channel = iter->second.lock(); channel)
                {
                    func(std::move(reply), std::move(channel));
                }
                else
                {
                    Log::error("Failed to lock channel with id: {}", channelId.value());
                    removeChannel(channelId);
                    return reply({{"error", "Failed to lock channel"}});
                }
            }
            else
            {
                Log::error("No channel found with id: {}", channelId.value());
                return reply({{"error", "No channel found with id"}});
            }
        });
    }

    template <typename FunctionT, typename ReplyCallable>
    void withSftpChannelDo(Ids::ChannelId channelId, FunctionT&& func, ReplyCallable&& reply)
    {
        within_strand_do([this,
                          channelId = std::move(channelId),
                          func = std::forward<FunctionT>(func),
                          reply = std::forward<ReplyCallable>(reply)]() mutable {
            if (auto iter = sftpChannels_.find(channelId); iter != sftpChannels_.end())
            {
                if (auto channel = iter->second.lock(); channel)
                {
                    func(std::move(reply), std::move(channel));
                }
                else
                {
                    Log::error("Failed to lock channel with id: {}", channelId.value());
                    removeSftpChannel(channelId);
                    return reply({{"error", "Failed to lock channel"}});
                }
            }
            else
            {
                Log::error("No channel found with id: {}", channelId.value());
                return reply({{"error", "No channel found with id"}});
            }
        });
    }

    void doOperationQueueWork();

  private:
    void resetQueueThrottle();

  private:
    Ids::SessionId id_;
    /// Has nothing to do with pause/unpause - this is used for shutdown of the session.
    std::atomic_bool running_;
    std::chrono::milliseconds operationThrottle_{5};
    bool queueThrottleTimerIsRunning_{false};
    int unthrottledLimitCounter_{0};
    std::unique_ptr<SecureShell::Session> session_{};
    std::unordered_map<Ids::ChannelId, std::weak_ptr<SecureShell::Channel>, Ids::IdHash> channels_{};
    std::unordered_map<Ids::ChannelId, std::weak_ptr<SecureShell::SftpSession>, Ids::IdHash> sftpChannels_{};
    std::shared_ptr<OperationQueue> operationQueue_;
};