#include <nui-file-explorer/support/recent_files.hpp>
#include <nui/utility/scope_exit.hpp>

#include <gtk/gtk.h>

namespace NuiFileExplorer
{
    RecentFileProvider::RecentFileProvider(Nui::RpcHub& hub)
        : hub_{&hub}
    {
        registerRpc();
    }
    void RecentFileProvider::registerRpc()
    {
        listRecent_ = std::make_unique<Nui::RpcHub::AutoUnregister>(hub_->autoRegisterFunction(
            "NuiFileExplorer::RecentFiles::list",
            [hub = hub_](std::string responseId)
            {
                GtkRecentManager* manager = gtk_recent_manager_get_default();
                GList* items = gtk_recent_manager_get_items(manager);

                auto freeStuff = Nui::ScopeExit(
                    [items]() noexcept
                    {
                        for (GList* l = items; l != nullptr; l = l->next)
                            gtk_recent_info_unref(static_cast<GtkRecentInfo*>(l->data));
                        g_list_free(items);
                    }
                );

                nlohmann::json entries = nlohmann::json::array();
                for (GList* l = items; l != nullptr; l = l->next)
                {
                    GtkRecentInfo* info = static_cast<GtkRecentInfo*>(l->data);

                    const char* uri = gtk_recent_info_get_uri(info);
                    const char* name = gtk_recent_info_get_display_name(info);
                    const char* mimeType = gtk_recent_info_get_mime_type(info);

                    entries.push_back({
                        {"uri", uri ? uri : ""},
                        {"name", name ? name : ""},
                        {"mimeType", mimeType ? mimeType : ""},
                    });
                }
                hub->callRemote(responseId, {{"success", true}, {"entries", entries}});
            }
        ));
    }
}