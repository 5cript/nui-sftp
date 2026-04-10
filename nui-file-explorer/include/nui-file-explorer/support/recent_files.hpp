#pragma once

#include <nui/rpc.hpp>

namespace NuiFileExplorer
{
    class RecentFileProvider
    {
      public:
        RecentFileProvider(Nui::RpcHub& hub);

      private:
        void registerRpc();

        Nui::RpcHub* hub_;
        std::unique_ptr<Nui::RpcHub::AutoUnregister> listRecent_{};
    };
}