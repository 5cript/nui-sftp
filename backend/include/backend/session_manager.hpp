#pragma once

#include <backend/session.hpp>
#include <backend/file_tracking/local_watch_manager.hpp>
#include <backend/ftp/ftp_engine_context.hpp>
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
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <expected>
#include <filesystem>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

/**
 * @brief The engine a SessionManager::connect call asks for: SSH without "engineType", else the one it names, SSH or
 *        FTP.
 *
 * @return Why it names none of them.
 */
std::expected<Persistence::TerminalEngineType, std::string> connectedEngineOf(nlohmann::json const& parameters);

class SessionManager
    : public std::enable_shared_from_this<SessionManager>
    , public RpcHelper::StrandRpc
{
  public:
    /**
     * @param ftpEngine Runs the FTP sessions, outlives the shutdown() of this manager.
     */
    SessionManager(
        boost::asio::any_io_executor executor,
        Persistence::StateHolder& stateHolder,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        FtpEngineContext& ftpEngine
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
     * @brief Makes the connection of a new session on the strand, handed the callback for its loss.
     */
    using Connect =
        std::function<std::expected<SessionConnection, std::string>(std::function<void()> onConnectionLoss)>;

    /**
     * @brief Connects a new session through @p connect, @p onComplete hears its id or why it failed. Refused once
     *        shutdown() closed the sessions.
     * @param sftpOptions The transfer options of the session.
     * @param failureToast Whether the error logged when it fails is shown as a toast.
     */
    void addSession(
        Connect connect,
        Persistence::SftpOptions sftpOptions,
        Log::Toast failureToast,
        std::function<void(std::optional<Ids::SessionId> const&, std::optional<std::string> const& errorMessage)>
            onComplete
    );

    /**
     * @brief Connects to the FTP server of @p sessionOptions, whose references are resolved. Asks the password
     *        providers for the password unless the login is anonymous.
     */
    Connect ftpConnect(Persistence::FtpSessionOptions sessionOptions);

    /**
     * @brief @p connect, which also sets @p passwordDeclined to whether a password prompt got no password during it, a
     *        prompt the user cancelled or no provider answered.
     */
    Connect noticingDeclinedPasswords(Connect connect, std::shared_ptr<bool> passwordDeclined);

    /**
     * @brief What a SessionManager::connect call connects with.
     */
    struct ConnectRequest
    {
        Persistence::TerminalEngineType engine{Persistence::TerminalEngineType::ssh};
        Connect connect{};
        /** @brief The transfer options of the session. */
        Persistence::SftpOptions sftpOptions{};
    };

    /**
     * @brief The connect that the parameters of a SessionManager::connect call ask for, see
     *        registerRpcSessionConnect(). Its connect sets @p passwordDeclined, see noticingDeclinedPasswords().
     *
     * @return Why the parameters name no engine to connect with or no options it can read.
     */
    std::expected<ConnectRequest, std::string>
    connectRequestOf(nlohmann::json const& parameters, std::shared_ptr<bool> passwordDeclined);

    /**
     * @brief Cancels pending and later password prompts, closes every session and refuses new ones, and waits until
     *        the strand has run its queued work. The FTP engine may go afterwards.
     *
     * Call before the window shuts down, never from within the strand.
     */
    void shutdown();

    friend int
    askPassDefault(char const* prompt, char* buf, std::size_t length, int echo, int verify, void* userdata) noexcept;

    // void startUpdateDispatching();
    // void stopUpdateDispatching();

  private:
    // void dispatchUpdates();

    /**
     * @brief Connects to the SSH server of @p sessionOptions, asking for passwords and key phrases through the
     *        password providers.
     */
    Connect secureShellConnect(Persistence::SshSessionOptions sessionOptions);

    /**
     * @brief Asks the password providers in the order of their priority until one answers. On the strand, which it
     *        blocks until then.
     *
     * @param whatFor What the prompt asks for, "password" or "keyPhrase".
     * @return The password, nothing when every provider declined, prompts are cancelled or asking failed.
     */
    std::optional<std::string> askPassword(std::string const& whatFor, std::string const& prompt) noexcept;

    /**
     * @brief Stops and removes every session, later ones are refused. On the strand.
     */
    void closeAllSessions();

    /**
     * Handles calls from the frontend to connect to a new server with the following payload:
     * {
     *     engineType: "ssh" or "ftp" (optional, default "ssh"),
     *     engineOptions: Persistence::SshSessionOptions or Persistence::FtpSessionOptions,
     *     showFailureToast: bool (optional, default true)
     * }
     * and answers {id} or {error, cancelled}, cancelled when a password prompt got no password, so retrying on its own
     * would only ask again.
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
     */
    void removeSession(Ids::SessionId sessionId);

    /**
     * @brief Stops the session and removes it. On the strand.
     * @param connectionLost The session lost its connection, its watches tell the frontend why they end.
     * @return False when there is no such session.
     */
    bool eraseSession(Ids::SessionId const& sessionId, bool connectionLost);

    /**
     * @brief Removes a session that lost its connection and tells the frontend, unless it was removed before. On the
     *        strand.
     */
    void onSessionLost(Ids::SessionId const& sessionId);

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
    /** @brief Outlives every session, see shutdown(). */
    FtpEngineContext* ftpEngine_{};
    /** @brief Set by shutdown() once the sessions are closed, new ones are refused from then on. On the strand. */
    bool sessionsClosed_{false};
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
    /** @brief The prompts askPassword got no password for, counted on the strand. */
    std::size_t declinedPasswordPrompts_{0u};
    std::unique_ptr<std::thread> addSessionThread_{};
    std::vector<SecureShell::PasswordCacheEntry> pwCache_{};
    /// Backups of bulk operations interrupted by lost connection; consumed
    /// by adoptBulkResume on reconnect or evicted via TTL.
    BulkResumeRegistry bulkResumeRegistry_{};
    /** @brief Shared with the queue of every session, so a kept transfer outlives a reconnect. */
    std::shared_ptr<KeptTransferRegistry> keptTransferRegistry_{std::make_shared<KeptTransferRegistry>()};
};

/**
 * @brief The libssh callback that asks the password providers, see SessionManager::askPassword(). Only on the strand of
 *        the SessionManager in the pair of @p userdata, with what the prompt asks for.
 *
 * @return 0 with the password in @p buf, -1 without one.
 */
int askPassDefault(char const* prompt, char* buf, std::size_t length, int echo, int verify, void* userdata) noexcept;