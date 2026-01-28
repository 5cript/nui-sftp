#pragma once

#include <backend/session.hpp>
#include <backend/password/password_provider.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state_holder.hpp>
#include <backend/rpc_helper.hpp>

#include <nui/rpc.hpp>
#include <libssh/libsshpp.hpp>
#include <libssh/sftp.h>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

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

    void addPasswordProvider(int priority, PasswordProvider* provider);
    void joinSessionAdder();
    void addSession(
        Persistence::SshSessionOptions const& sessionOptions,
        std::function<void(std::optional<Ids::SessionId> const&, std::optional<std::string> const& errorMessage)>
            onComplete
    );

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

    /// Removes a session and closes all its channels. Safe to call from any thread.
    void removeSession(Ids::SessionId sessionId);

  private:
    Persistence::StateHolder* stateHolder_{};
    std::unordered_map<Ids::SessionId, std::shared_ptr<Session>, Ids::IdHash> sessions_{};

    std::map<int, PasswordProvider*> passwordProviders_{};
    std::unique_ptr<std::thread> addSessionThread_{};
    std::vector<SecureShell::PasswordCacheEntry> pwCache_{};
    std::atomic_bool updateDispatchRunning_{false};
};

int askPassDefault(char const* prompt, char* buf, std::size_t length, int echo, int verify, void* userdata);