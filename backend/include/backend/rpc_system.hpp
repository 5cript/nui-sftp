#pragma once

#include <backend/rpc_helper.hpp>
#include <persistence/state/local_filesystem_options.hpp>

class RpcSystem : public RpcHelper::StrandRpc
{
  public:
    RpcSystem(boost::asio::any_io_executor executor, Nui::Window& wnd, Nui::RpcHub& hub);

  private:
    void registerGetUsername();

    std::string username() const;

    mutable std::string usernameMemo_;
};