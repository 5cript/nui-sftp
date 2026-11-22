#pragma once

#include <nui/rpc.hpp>

#include <memory>

namespace NuiFileExplorer
{
    /**
     * @brief Backend support class that resolves system place paths (XDG directories on Linux,
     * Windows known folders on Windows) and exposes them via an RPC endpoint.
     *
     * On Linux the XDG_*_DIR environment variables take precedence over user-dirs.dirs, and only directories that
     * exist are listed.
     *
     * Registers: @c "NuiFileExplorer::DefaultPlaces::list"
     * Returns a JSON array of @c {kind, name, path} objects. @c kind is a stable id (home, desktop, downloads,
     * documents, pictures, videos, movies, music) to pick icons and translations by, @c name is an English label.
     *
     * Construct this in the backend Main once the hub is alive, the endpoint lives as long as the provider.
     */
    class DefaultPlacesProvider
    {
      public:
        explicit DefaultPlacesProvider(Nui::RpcHub& hub);
        ~DefaultPlacesProvider();

        DefaultPlacesProvider(DefaultPlacesProvider const&) = delete;
        DefaultPlacesProvider& operator=(DefaultPlacesProvider const&) = delete;
        DefaultPlacesProvider(DefaultPlacesProvider&&) = delete;
        DefaultPlacesProvider& operator=(DefaultPlacesProvider&&) = delete;

      private:
        std::unique_ptr<Nui::RpcHub::AutoUnregister> listPlaces_{};
    };
}
