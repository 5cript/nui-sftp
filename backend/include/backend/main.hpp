#pragma once

#include <backend/process/process_store.hpp>
#include <backend/session_manager.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <persistence/state_holder.hpp>
#include <backend/rpc_filesystem.hpp>
#include <backend/rpc_system.hpp>
#include <backend/password/password_prompter.hpp>
#include <backend/theme_finder.hpp>
#include <backend/program_options.hpp>
#include <backend/opener.hpp>
#include <ssh/async/processing_thread.hpp>
#include <events/app_wide_events.hpp>
#include <nui-file-explorer/support/default_places.hpp>
#ifdef _WIN32
#    include <nui-file-explorer/support/windows_drives.hpp>
#endif

#include <boost/asio/steady_timer.hpp>
#include <nui/core.hpp>
#include <nui/rpc.hpp>
#include <nui/window.hpp>
#include <roar/mime_type.hpp>
#include <efsw/efsw.hpp>

#include <filesystem>
#include <atomic>
#include <mutex>

class ForkPool;

class Main
{
  public:
    Main(ProgramOptions options, ForkPool* forkPool = nullptr);
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
    std::unique_ptr<Opener> opener_;
    std::unique_ptr<RpcFilesystem> rpcFilesystem_;
    std::unique_ptr<RpcSystem> rpcSystem_;
    ProcessStore processes_;
    PasswordPrompter prompter_;
    std::unique_ptr<FileTracking::TempDirInstanceManager> tempDirInstanceManager_;

    std::shared_ptr<SessionManager> sshSessionManager_;
    boost::asio::steady_timer childSignalTimer_;
    // for display later in UI
    std::string initialPersistenceLoadWarning_;
    AppWideEvents events_;
    ThemeFinder themeFinder_;
    std::once_flag rpcAliveOnce_;

    std::unique_ptr<NuiFileExplorer::DefaultPlacesProvider> defaultPlacesProvider_;
#ifdef _WIN32
    std::unique_ptr<NuiFileExplorer::WindowsDrivesProvider> windowsDrivesProvider_;
#endif

    struct PlatformSpecifics;
    std::unique_ptr<PlatformSpecifics> platformSpecifics_;
};