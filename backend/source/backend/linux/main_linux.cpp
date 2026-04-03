// Nothing yet

#include <backend/linux/main_linux.hpp>
#include <gtk/gtk.h>

#if GTK_MAJOR_VERSION >= 4
#    include <jsc/jsc.h>
#    include <webkit/webkit.h>
#    ifdef GDK_WINDOWING_X11
#        include <gdk/x11/gdkx.h>
#    endif
#elif GTK_MAJOR_VERSION >= 3
#    include <JavaScriptCore/JavaScript.h>
#    include <webkit2/webkit2.h>
#endif

namespace
{
    bool isFilteredAction(WebKitContextMenuAction action)
    {
        switch (action)
        {
            case WEBKIT_CONTEXT_MENU_ACTION_RELOAD:
            case WEBKIT_CONTEXT_MENU_ACTION_GO_BACK:
            case WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD:
            case WEBKIT_CONTEXT_MENU_ACTION_STOP:
                return true;
            default:
                return false;
        }
    }
}

gboolean on_context_menu(
    WebKitWebView* /*web_view*/,
    WebKitContextMenu* context_menu,
    GdkEvent* /*event*/,
    WebKitHitTestResult* /*hit_test_result*/,
    gpointer /*user_data*/
)
{
    GList* items = webkit_context_menu_get_items(context_menu);
    GList* copy = g_list_copy(items);
    for (GList* l = copy; l != NULL; l = l->next)
    {
        auto* item = static_cast<WebKitContextMenuItem*>(l->data);
        if (isFilteredAction(webkit_context_menu_item_get_stock_action(item)))
            webkit_context_menu_remove(context_menu, item);
    }
    g_list_free(copy);
    return FALSE; // show modified menu
}

Main::PlatformSpecifics::PlatformSpecifics(Nui::Window& wnd, Nui::RpcHub&)
{
    auto* webview = static_cast<GtkWidget*>(wnd.getNativeWebView());
    g_signal_connect(
        webview,
        "context-menu",
        G_CALLBACK(
            +[](WebKitWebView* web_view,
                 WebKitContextMenu* context_menu,
                 GdkEvent* event,
                 WebKitHitTestResult* hit_test_result,
                 gpointer user_data) -> gboolean
            {
                return on_context_menu(web_view, context_menu, event, hit_test_result, user_data);
            }
        ),
        nullptr
    );
}