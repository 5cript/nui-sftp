#pragma once

#include <backend/rpc_helper.hpp>
#include <backend/opener.hpp>
#include <persistence/state/local_filesystem_options.hpp>

class RpcFilesystem : public RpcHelper::StrandRpc
{
  public:
    RpcFilesystem(
        boost::asio::any_io_executor executor,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::LocalFilesystemOptions options,
        Opener& opener
    );

  private:
    void registerRemove();
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
};