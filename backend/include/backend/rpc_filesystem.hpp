#pragma once

#include <backend/rpc_helper.hpp>
#include <persistence/state/local_filesystem_options.hpp>

class RpcFilesystem : public RpcHelper::StrandRpc
{
  public:
    RpcFilesystem(
        boost::asio::any_io_executor executor,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::LocalFilesystemOptions options
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

  private:
    Persistence::LocalFilesystemOptions options_;
};