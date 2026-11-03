#pragma once

#include <ssh/session.hpp>
#include <remote_io/connection.hpp>
#include <remote_io/remote_file_system.hpp>
#include <ids/ids.hpp>
#include <backend/sftp/operation_queue.hpp>
#include <backend/local_operation_queue.hpp>
#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/sftp/kept_transfer_registry.hpp>
#include <backend/file_tracking/tracker.hpp>
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
 * @brief What a session talks to its server through.
 */
struct SessionConnection
{
    std::unique_ptr<RemoteIo::IConnection> connection;
    /** @brief The SSH session behind the connection, which hosts terminals. Nothing for a protocol without them. */
    SecureShell::Session* terminalHost{nullptr};

    /**
     * @brief An SSH session, which is the connection and hosts terminals as well.
     */
    static SessionConnection fromSecureShell(std::unique_ptr<SecureShell::Session> session)
    {
        auto* const terminalHost = session.get();
        return {.connection = std::move(session), .terminalHost = terminalHost};
    }
};

/**
 * @brief This session is the implementation equivalent of one tab in the UI.
 * A tab in the UI represents everything related to one server address and port.
 *
 * These sessions are managed by SessionManager. Each session has an id, ssh channels and file system channels.
 */
class Session
    : public RpcHelper::StrandRpc
    , public std::enable_shared_from_this<Session>
{
  public:
    constexpr static auto futureTimeout = std::chrono::seconds{10};

    Session(
        Ids::SessionId id,
        SessionConnection connection,
        boost::asio::any_io_executor executor,
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::SftpOptions const& sftpOptions,
        BulkResumeRegistry* bulkResumeRegistry,
        std::shared_ptr<KeptTransferRegistry> keptTransferRegistry,
        std::shared_ptr<LocalOperationQueue> localOperationQueue,
        FileTracking::TrackerServices fileTracking = {}
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

    /**
     * @brief Ends the watches of this session without telling the frontend, which is gone, and releases the instances
     *        of its copies, see FileTracking::Tracker::stopAll(). Must be called from the strand.
     */
    void stopFileTracking();

    /**
     * @brief The connection of this session was lost: its watches end and the frontend shows why, see
     *        FileTracking::Tracker::connectionLost(). Must be called from the strand, before stop().
     */
    void onConnectionLost();

  private:
    /**
     * @brief The server side of the file tracker: this session's operation queue and file system channels.
     */
    class FileTrackingRemote;

    /**
     * @brief Creates the file tracker of this session, the strand runs it.
     */
    void startFileTracking();

    /**
     * @brief Handles the requests of the file tracking panel, "FileTracker::{id}::..." with the paths as UTF-8:
     *        open (remote path, size, watch) answers once the copy is there with {instanceId, localPath}, an error to
     *        show or neither, checkUploadAndWatch (local item, remote directory) with {item, destination},
     *        uploadAndWatch (local item, remote destination), stop (watch id), setOption (watch id, option, value),
     *        discard (instance id, local path) with {instanceDestroyed}, the answers to conflicts, retrying and
     *        dismissing failures, removing stopped watches and listing and removing orphaned copies. See
     *        FileTracking::Tracker.
     */
    void registerRpcFileTracker();

    /**
     * @brief An open file system channel of this session, nothing when there is none.
     */
    std::shared_ptr<RemoteIo::IRemoteFileSystem> anySftpChannel() const;

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
     * Handles calls from the frontend to list a remote directory with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpListDirectory();

    /**
     * Handles calls from the frontend to get the remote home directory with the following payload:
     * {
     *     sftpChannelId: string
     * }
     */
    void registerRpcSftpHomeDirectory();

    /**
     * Handles calls from the frontend to read the remote XDG user directories with the following payload:
     * {
     *     sftpChannelId: string,
     *     home: string
     * }
     */
    void registerRpcSftpUserDirectories();

    /**
     * Handles calls from the frontend to create a remote directory with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpCreateDirectory();

    /**
     * Handles calls from the frontend to create a remote file with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpCreateFile();

    /**
     * Handles calls from the frontend to delete remote files with the following payload:
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
     * Handles calls from the frontend to rename a remote file with the following payload:
     * {
     *     sftpChannelId: string,
     *     oldName: string,
     *     newName: string
     * }
     */
    void registerRpcSftpRename();

    /**
     * Handles calls from the frontend to stat a remote file with the following payload:
     * {
     *     sftpChannelId: string,
     *     path: string
     * }
     */
    void registerRpcSftpStat();

    /**
     * Handles calls from the frontend to stat a remote file with the following payload:
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
     * @brief Handles "Session::{id}::sftp::continueKept" (channel id, operation id) and
     *        "Session::{id}::sftp::discardKept" (channel id, operation ids): goes on with a transfer the user canceled
     *        keeping its partial files, or deletes the partial files of such transfers, see
     *        OperationQueue::continueKept and OperationQueue::discardKept.
     */
    void registerRpcSftpKeptTransfers();

    /**
     * @brief Handles "Session::{id}::addLocalDelete" (operation id, local paths): queues the recursive delete of the
     *        local items in the local operation queue, held while the queue of this session is paused, see
     *        LocalOperationQueue::addExplorerDelete.
     */
    void registerRpcAddLocalDeleteOperation();
    void registerRpcAddLocalCopyOperation();
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
     * @brief Blocks the strand until the processing thread of @p channel ran what is queued there: the operation
     *        queue's batch and the calls that answer from a completion. For a call that then waits for that thread with
     *        a timeout, which would count them otherwise.
     */
    static void waitForProcessingThread(RemoteIo::IRemoteFileSystem const& channel);

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

    /**
     * @brief withLockedChannelDo for the file system channels. A call that waits for the file system with a timeout
     *        calls waitForProcessingThread first.
     */
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
     * @brief Queues operations on the file system channel @p channelId with @p queue, called on the strand with the
     *        operation queue and the channel, and answers @p reply with the JSON it returns or with the message it
     *        fails with, which the log names as the failure to queue @p description. A queued request wakes the queue.
     */
    template <typename QueueT>
    void queueOnChannel(RpcHelper::RpcOnce&& reply, Ids::ChannelId channelId, std::string description, QueueT&& queue)
    {
        withSftpChannelDo(
            std::move(channelId),
            [description = std::move(description), queue = std::forward<QueueT>(queue)](
                RpcHelper::RpcOnce&& reply, Session& self, std::shared_ptr<RemoteIo::IRemoteFileSystem> const& channel
            ) mutable
            {
                if (!self.operationQueue_)
                    return reply.error("Session no longer exists");
                // Queueing asks the server with a timeout.
                waitForProcessingThread(*channel);
                std::expected<nlohmann::json, std::string> queued = queue(*self.operationQueue_, channel);
                if (!queued)
                {
                    Log::error("Failed to queue {}: {}", description, queued.error());
                    return reply.error(queued.error());
                }
                Log::info("Queued {}", description);
                self.wakeOperationQueue();
                reply(*std::move(queued));
            },
            std::move(reply)
        );
    }

    /**
     * @brief Works the operation queue once on the strand, again right away while an operation made progress.
     */
    void doOperationQueueWork();

    /**
     * @brief Works the operation queue soon, unless that is already scheduled. Callable from any thread.
     */
    void wakeOperationQueue();

    /**
     * @brief Posts doOperationQueueWork unless it is posted already. On the strand.
     */
    void scheduleOperationQueueWork();

  private:
    Ids::SessionId id_;
    /// Has nothing to do with pause/unpause - this is used for shutdown of the session.
    std::atomic_bool running_;
    /** @brief A doOperationQueueWork is posted and not yet run. On the strand. */
    bool operationQueueWorkScheduled_{false};
    std::unique_ptr<RemoteIo::IConnection> connection_{};
    /** @brief Part of connection_, nothing without terminals or once connection_ is gone. */
    SecureShell::Session* terminalHost_{nullptr};
    std::shared_ptr<OperationQueue> operationQueue_;
    /// Non-owning. Lifetime is the SessionManager's, which outlives all
    /// sessions it created.
    BulkResumeRegistry* bulkResumeRegistry_{};
    std::unordered_map<Ids::ChannelId, std::weak_ptr<SecureShell::Channel>, Ids::IdHash> channels_{};
    std::unordered_map<Ids::ChannelId, std::weak_ptr<RemoteIo::IRemoteFileSystem>, Ids::IdHash> sftpChannels_{};
    /** @brief The temp file suffix of uploads, which names where a replaced item is kept during a rename. */
    std::string remoteTempFileSuffix_;
    /** @brief Runs the local deletes of this session, which go on when it ends. */
    std::shared_ptr<LocalOperationQueue> localOperationQueue_;
    FileTracking::TrackerServices fileTrackingServices_;
    /** @brief Suffix of this session's downloads in progress, the file tracker ignores them. */
    std::string downloadTempFileSuffix_;
    std::unique_ptr<FileTracking::Tracker> tracker_{};
};