#pragma once

#include <nui/rpc.hpp>

namespace NuiFileExplorer
{
    /**
     * @brief Backend support class (Windows only) that enumerates mounted drives/volumes and
     * exposes them via an RPC endpoint.
     *
     * Registers: @c "NuiFileExplorer::Drives::list"
     * Returns a JSON array of @c {name, path} objects where @c name is the volume label (with
     * drive letter suffix) and @c path is the root path (e.g. @c "C:/").
     *
     * Construct this in the backend Main and call @c registerRpc() once the hub is alive.
     */
    class WindowsDrivesProvider
    {
      public:
        explicit WindowsDrivesProvider(Nui::RpcHub& hub);
        ~WindowsDrivesProvider();

        WindowsDrivesProvider(WindowsDrivesProvider const&) = delete;
        WindowsDrivesProvider& operator=(WindowsDrivesProvider const&) = delete;
        WindowsDrivesProvider(WindowsDrivesProvider&&) = delete;
        WindowsDrivesProvider& operator=(WindowsDrivesProvider&&) = delete;

      private:
        void registerRpc();

        Nui::RpcHub* hub_;
        std::unique_ptr<Nui::RpcHub::AutoUnregister> listDrives_;
    };
}
