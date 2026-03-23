#pragma once

#include <backend/process/process_store.hpp>
#include <backend/session_manager.hpp>
#include <persistence/state_holder.hpp>
#include <backend/rpc_filesystem.hpp>
#include <backend/rpc_system.hpp>
#include <backend/password/password_prompter.hpp>
#include <backend/theme_finder.hpp>
#include <ssh/async/processing_thread.hpp>
#include <events/app_wide_events.hpp>

#include <boost/asio/steady_timer.hpp>
#include <nui/core.hpp>
#include <nui/rpc.hpp>
#include <nui/window.hpp>
#include <roar/mime_type.hpp>
#include <efsw/efsw.hpp>

#include <filesystem>
#include <atomic>
#include <mutex>

class Main
{
  public:
    Main(int const argc, char const* const* argv);
    ~Main();

    Main(Main const&) = delete;
    Main& operator=(Main const&) = delete;
    Main(Main&&) = delete;
    Main& operator=(Main&&) = delete;

    void registerRpc();
    void show();
    void startChildSignalTimer();

  private:
    void registerInitialWarningGetter();
    void onRpcAlive();

  private:
    std::atomic_bool shuttingDown_;
    std::filesystem::path programDir_;
    Persistence::StateHolder stateHolder_;
    struct LoggerSetup
    {
        LoggerSetup(Persistence::StateHolder& stateHolder);
    } loggerSetup_;
    Nui::Window window_;
    Nui::RpcHub hub_;
    std::unique_ptr<RpcFilesystem> rpcFilesystem_;
    std::unique_ptr<RpcSystem> rpcSystem_;
    ProcessStore processes_;
    PasswordPrompter prompter_;
    std::shared_ptr<SessionManager> sshSessionManager_;
    boost::asio::steady_timer childSignalTimer_;
    // for display later in UI
    std::string initialPersistenceLoadWarning_;
    AppWideEvents events_;
    ThemeFinder themeFinder_;
    std::once_flag rpcAliveOnce_;

    struct PlatformSpecifics;
    std::unique_ptr<PlatformSpecifics> platformSpecifics_;
};