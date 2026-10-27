#pragma once

#include <backend/session.hpp>
#include <backend/file_tracking/local_watch_manager.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <backend/local_operation_queue.hpp>
#include <backend/password/password_provider.hpp>
#include <backend/password/password_request.hpp>
#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/sftp/kept_transfer_registry.hpp>
#include <log/toast.hpp>
#include <persistence/state/file_tracking_options.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state_holder.hpp>
#include <shared_data/file_tracking/watch_options.hpp>
#include <backend/rpc_helper.hpp>

#include <nui/rpc.hpp>
#include <libssh/libsshpp.hpp>
#include <libssh/sftp.h>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include <thread>

class SessionManager
    : public std::enable_shared_from_this<SessionManager>
    , public RpcHelper::StrandRpc
{
  public:
    SessionManager(
        boost::asio::any_io_executor executor,
        Persistence::StateHolder& stateHolder,
        Nui::Window& wnd,
        Nui::RpcHub& hub
    );
    ~SessionManager();
    SessionManager(SessionManager const&) = delete;
    SessionManager& operator=(SessionManager const&) = delete;
    SessionManager(SessionManager&&) = delete;
    SessionManager& operator=(SessionManager&&) = delete;

    void registerRpc();

    /**
     * @brief Whether the sessions refuse local deletes, as the local filesystem options say. Refused until set.
     */
    void preventLocalDeletion(bool prevent);

    /**
     * @brief Starts file tracking on the strand of the sessions: the instances of temporary downloads below
     *        @p temporaryDownloadsDirectory and the watches of local items. Call once, before the first session.
     */
    void startFileTracking(std::filesystem::path const& temporaryDownloadsDirectory);

    /**
     * @brief The options a new watch starts with from now on. Safe to call from any thread.
     */
    void fileTrackingDefaults(Persistence::FileTrackingOptions const& options);

    void addPasswordProvider(int priority, PasswordProvider* provider);
    void joinSessionAdder();
    /**
     * @brief Connects a new session with @p sessionOptions, @p onComplete hears its id or why it failed.
     * @param failureToast Whether the error logged when it fails is shown as a toast.
     */
    void addSession(
        Persistence::SshSessionOptions const& sessionOptions,
        Log::Toast failureToast,
        std::function<void(std::optional<Ids::SessionId> const&, std::optional<std::string> const& errorMessage)>
            onComplete
    );

    /**
     * @brief Cancels pending and later password prompts and waits until the strand has run its queued work.
     *
     * Call before the window shuts down, never from within the strand.
     */
    void shutdown();

    friend int askPassDefault(char const* prompt, char* buf, std::size_t length, int echo, int verify, void* userdata);

    // void startUpdateDispatching();
    // void stopUpdateDispatching();

  private:
    // void dispatchUpdates();

    /**
     * Handles calls from the frontend to connect to a new ssh server with the following payload:
     * {
     *     engine: Persistence::SshTerminalEngine
     * }
     */
    void registerRpcSessionConnect();

    /**
     * Handles calls from the frontend to disconnect from an ssh server with the following payload:
     * {
     *     sessionId: string
     * }
     */
    void registerRpcSessionDisconnect();

    /**
     * @brief Removes a session and closes all its channels. Safe to call from any thread.
     * @param connectionLost The session lost its connection, its watches tell the frontend why they end.
     */
    void removeSession(Ids::SessionId sessionId, bool connectionLost = false);

    /**
     * @brief Handles "Session::sftp::adoptBulkResume" — pulls saved bulk
     *        operations from @c bulkResumeRegistry_ and re-enqueues them
     *        into the named session's OperationQueue.  Called from
     *        Session::applySnapshot during a seamless reconnect.
     */
    void registerRpcAdoptBulkResume();

    /**
     * @brief Handles "Session::sftp::discardBulkResume" — removes saved
     *        bulk-operation backups by id without consuming them.  Called
     *        from the dying frontend Session's destructor when the user
     *        abandons the reconnect.
     */
    void registerRpcDiscardBulkResume();

    /**
     * @brief Handles "FileTracking::countOrphaned" with {count} and "FileTracking::forceCleanup" with {removed}: the
     *        instance directories no live process holds, which the latter removes.
     */
    void registerRpcOrphanedInstances();

    /**
     * @brief Handles "FileTracker::stopAll": a frontend that loads ends the watches of the one before it (a reload),
     *        nobody is left to see them.
     */
    void registerRpcStopFileTracking();

    /**
     * @brief Makes a new request the pending one, so that shutdown() can cancel it.
     *
     * @return The request, or nullptr once prompts are cancelled.
     */
    std::shared_ptr<PasswordRequest> beginPasswordRequest();

    /**
     * @brief Clears the pending request set by beginPasswordRequest().
     */
    void endPasswordRequest() noexcept;

  private:
    std::atomic_bool updateDispatchRunning_{false};
    Persistence::StateHolder* stateHolder_{};
    /** @brief Outlive the sessions, which watch through them. */
    std::unique_ptr<FileTracking::TempDirInstanceManager> temporaryInstances_{};
    std::unique_ptr<FileTracking::LocalWatchManager> localWatches_{};
    /** @brief Read and written on the strand. */
    SharedData::WatchOptions fileTrackingDefaults_{};
    std::unordered_map<Ids::SessionId, std::shared_ptr<Session>, Ids::IdHash> sessions_{};
    /** @brief Read by the local operation queue on each local delete the explorer asks for. */
    std::shared_ptr<std::atomic_bool> localDeletionPrevented_{std::make_shared<std::atomic_bool>(true)};
    /** @brief Shared with every session, outlives them all, so a local delete never ends with its session. */
    std::shared_ptr<LocalOperationQueue> localOperationQueue_;

    std::map<int, PasswordProvider*> passwordProviders_{};
    std::mutex passwordRequestMutex_{};
    /** @brief At most one, askPassDefault only runs within the strand. */
    std::shared_ptr<PasswordRequest> pendingPasswordRequest_{};
    bool passwordPromptsCancelled_{false};
    std::unique_ptr<std::thread> addSessionThread_{};
    std::vector<SecureShell::PasswordCacheEntry> pwCache_{};
    /// Backups of bulk operations interrupted by lost connection; consumed
    /// by adoptBulkResume on reconnect or evicted via TTL.
    BulkResumeRegistry bulkResumeRegistry_{};
    /** @brief Shared with the queue of every session, so a kept transfer outlives a reconnect. */
    std::shared_ptr<KeptTransferRegistry> keptTransferRegistry_{std::make_shared<KeptTransferRegistry>()};
};

int askPassDefault(char const* prompt, char* buf, std::size_t length, int echo, int verify, void* userdata);