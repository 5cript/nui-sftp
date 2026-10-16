#pragma once

#include <backend/rpc_helper.hpp>
#include <backend/opener.hpp>
#include <persistence/state/local_filesystem_options.hpp>

#include <functional>

class RpcFilesystem : public RpcHelper::StrandRpc
{
  public:
    RpcFilesystem(
        boost::asio::any_io_executor executor,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::LocalFilesystemOptions options,
        Opener& opener,
        std::function<Persistence::RiskyFilePolicy()> riskyFilePolicy = []() { return Persistence::RiskyFilePolicy::Warn; }
    );

  private:
    void registerRemoveMultiple();
    void registerRename();
    void registerListFiles();
    void registerCreateFile();
    void registerCreateDirectory();
    void registerProperties();
    void registerGetHome();
    void registerDoesExist();
    void registerDoesExistBatch();
    void registerWriteFile();
    void registerOpen();
    void registerOpenInFileManager();
    void registerOpenerCapabilities();

  private:
    Persistence::LocalFilesystemOptions options_;
    Opener* opener_;
    /** @brief Read on every open, so a changed setting applies at once. */
    std::function<Persistence::RiskyFilePolicy()> riskyFilePolicy_;
};