#pragma once

#include <ssh/session.hpp>
#include <ssh/sftp_session.hpp>
#include <ids/ids.hpp>
#include <backend/sftp/operation_queue.hpp>
#include <backend/local_operation_queue.hpp>
#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/rpc_helper.hpp>
#include <persistence/state_holder.hpp>

#include <atomic>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

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
        Persistence::SftpOptions const& sftpOptions,
        BulkResumeRegistry* bulkResumeRegistry,
        std::shared_ptr<LocalOperationQueue> localOperationQueue
    );

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session();

    void start();
    void stop();

    /**
     * @brief Adopts previously-saved bulk operations into this session's
     *        OperationQueue.  Each id is looked up in the registry; missing
     *        entries (already evicted or never saved) are silently skipped.
     *        Used by SessionManager::adoptBulkResume to drive the
     *        seamless-reconnect resume path.
     */
    void adoptBulkResumes(std::vector<Ids::OperationId> const& operationIds);

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
     * Handles calls from the frontend to get the remote home directory over sftp with the following payload:
     * {
     *     sftpChannelId: string
     * }
     */
    void registerRpcSftpHomeDirectory();

    /**
     * Handles calls from the frontend to read the XDG user directories over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     home: string
     * }
     */
    void registerRpcSftpUserDirectories();

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

    /**
     * Handles calls from the frontend to delete files over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     paths: vector<string>
     * }
     */
    void registerRpcSftpDeleteFiles();

    /**
     * Checks if non empty directories are involved before deletion with the following payload:
     * {
     *     sftpChannelId: string,
     *     directories: vector<string>
     * }
     */
    void registerRpcSftpPreDeleteChecks();

    /**
     * Handles calls from the frontend to rename a file over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     oldName: string,
     *     newName: string
     * }
     */
    void registerRpcSftpRename();

    /**
     * Handles calls from the frontend to stat a file over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpStat();

    /**
     * Handles calls from the frontend to stat a file over sftp with the following payload:
     * {
     *     sftpChannelId: string,
     *     paths: vector<path>
     *     recursive: bool
     * }
     */
    void registerRpcQueuedRemoves();

    void registerRpcSftpAddDownloadOperation();
    void registerRpcSftpAddUploadOperation();
    void registerRpcSftpAddArchiveDownloadOperation();
    void registerRpcSftpAddArchiveUploadOperation();
    void registerRpcSftpAddRenameOperation();
    void registerRpcSftpAddBulkDownloadOperation();
    void registerRpcSftpAddBulkUploadOperation();
    void registerRpcSftpAddBulkDeleteOperation();

    /**
     * @brief Handles "Session::{id}::addLocalDelete" (operation id, local paths): queues the recursive delete of the
     *        local items in the local operation queue, held while the queue of this session is paused, see
     *        LocalOperationQueue::addExplorerDelete.
     */
    void registerRpcAddLocalDeleteOperation();
    void registerRpcSftpExistsBatch();
    void registerRpcSftpOpenSyncSession();
    void registerRpcSftpRecomputeSyncDiff();
    void registerRpcSftpLoadSyncDiffChildren();
    void registerRpcSftpExecuteSyncSelection();
    void registerRpcSftpCancelSyncDiff();
    void registerRpcSftpCloseSyncSession();
    void registerOperationQueuePauseUnpause();

    void removeChannel(Ids::ChannelId channelId);

    void removeSftpChannel(Ids::ChannelId channelId);

    /**
     * @brief Runs @p func on the strand with the channel @p channelId of @p channels, as (reply, channel) or
     *        (reply, session, channel). @p reply is always answered: when the session is gone, when there is no such
     *        channel, and with the message of an exception @p func throws.
     */
    template <typename ChannelT, typename FunctionT>
    void withLockedChannelDo(
        std::unordered_map<Ids::ChannelId, std::weak_ptr<ChannelT>, Ids::IdHash> Session::* channels,
        void (Session::*remove)(Ids::ChannelId),
        Ids::ChannelId channelId,
        FunctionT&& func,
        RpcHelper::RpcOnce&& reply
    )
    {
        within_strand_do(
            [weak = weak_from_this(),
                channels,
                remove,
                channelId = std::move(channelId),
                func = std::forward<FunctionT>(func),
                reply = std::move(reply)]() mutable
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");
                auto& map = (*self).*channels;
                const auto iter = map.find(channelId);
                if (iter == map.end())
                {
                    Log::error("No channel found with id: {}", channelId.value());
                    return reply.error("No channel found with id");
                }
                auto channel = iter->second.lock();
                if (!channel)
                {
                    Log::error("Failed to lock channel with id: {}", channelId.value());
                    ((*self).*remove)(channelId);
                    return reply.error("Failed to lock channel");
                }
                try
                {
                    if constexpr (
                        std::is_invocable_v<FunctionT&, RpcHelper::RpcOnce&&, Session&, std::shared_ptr<ChannelT>>
                    )
                        func(std::move(reply), *self, std::move(channel));
                    else
                        func(std::move(reply), std::move(channel));
                }
                catch (std::exception const& exception)
                {
                    Log::error(
                        "A call on channel '{}' failed: {}", channelId.value(), Utility::errorMessage(exception)
                    );
                    reply.error(Utility::errorMessage(exception));
                }
                catch (...)
                {
                    Log::error("A call on channel '{}' failed with an unknown exception", channelId.value());
                    reply.error("Unknown error");
                }
            }
        );
    }

    template <typename FunctionT>
    void withChannelDo(Ids::ChannelId channelId, FunctionT&& func, RpcHelper::RpcOnce&& reply)
    {
        withLockedChannelDo(
            &Session::channels_,
            &Session::removeChannel,
            std::move(channelId),
            std::forward<FunctionT>(func),
            std::move(reply)
        );
    }

    template <typename FunctionT>
    void withSftpChannelDo(Ids::ChannelId channelId, FunctionT&& func, RpcHelper::RpcOnce&& reply)
    {
        withLockedChannelDo(
            &Session::sftpChannels_,
            &Session::removeSftpChannel,
            std::move(channelId),
            std::forward<FunctionT>(func),
            std::move(reply)
        );
    }

    /**
     * @brief Queues operations on the SFTP channel @p channelId with @p queue, called on the strand with the operation
     *        queue and the channel, and answers @p reply with the JSON it returns or with the message it fails with,
     *        which the log names as the failure to queue @p description. A queued request resets the queue throttle.
     */
    template <typename QueueT>
    void queueOnChannel(RpcHelper::RpcOnce&& reply, Ids::ChannelId channelId, std::string description, QueueT&& queue)
    {
        withSftpChannelDo(
            std::move(channelId),
            [description = std::move(description), queue = std::forward<QueueT>(queue)](
                RpcHelper::RpcOnce&& reply, Session& self, std::shared_ptr<SecureShell::SftpSession> const& channel
            ) mutable
            {
                if (!self.operationQueue_)
                    return reply.error("Session no longer exists");
                std::expected<nlohmann::json, std::string> queued = queue(*self.operationQueue_, *channel);
                if (!queued)
                {
                    Log::error("Failed to queue {}: {}", description, queued.error());
                    return reply.error(queued.error());
                }
                Log::info("Queued {}", description);
                self.resetQueueThrottle();
                reply(*std::move(queued));
            },
            std::move(reply)
        );
    }

    void doOperationQueueWork();
    void resetQueueThrottle();

  private:
    Ids::SessionId id_;
    /// Has nothing to do with pause/unpause - this is used for shutdown of the session.
    std::atomic_bool running_;
    std::chrono::milliseconds operationThrottle_{5};
    bool queueThrottleTimerIsRunning_{false};
    int unthrottledLimitCounter_{0};
    std::unique_ptr<SecureShell::Session> session_{};
    std::shared_ptr<OperationQueue> operationQueue_;
    /// Non-owning. Lifetime is the SessionManager's, which outlives all
    /// sessions it created.
    BulkResumeRegistry* bulkResumeRegistry_{};
    std::unordered_map<Ids::ChannelId, std::weak_ptr<SecureShell::Channel>, Ids::IdHash> channels_{};
    std::unordered_map<Ids::ChannelId, std::weak_ptr<SecureShell::SftpSession>, Ids::IdHash> sftpChannels_{};
    /** @brief The temp file suffix of uploads, which names where a replaced item is kept during a rename. */
    std::string remoteTempFileSuffix_;
    /** @brief Runs the local deletes of this session, which go on when it ends. */
    std::shared_ptr<LocalOperationQueue> localOperationQueue_;
};