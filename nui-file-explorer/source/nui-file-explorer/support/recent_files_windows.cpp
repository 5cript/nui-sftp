#include <nui-file-explorer/support/recent_files.hpp>

namespace NuiFileExplorer
{
    RecentFileProvider::RecentFileProvider(Nui::RpcHub& hub)
        : hub_{&hub}
    {
        registerRpc();
    }
    void RecentFileProvider::registerRpc()
    {
        listRecent_ = hub_->autoRegisterFunction(
            "NuiFileExplorer::RecentFiles::list",
            [hub = hub_](std::string responseId)
            {
                // TODO: Implement, reply with not implemented error for now:
                std::string message = "Not implemented";
                hub->callRemote(responseId, {{"success", false}, {"error", message}});
            }
        );
    }
}