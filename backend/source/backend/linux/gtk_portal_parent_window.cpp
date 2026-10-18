#include <backend/linux/gtk_portal_parent_window.hpp>

#include <log/log.hpp>

#include <gtk/gtk.h>

#if GTK_MAJOR_VERSION >= 4
#    ifdef GDK_WINDOWING_X11
#        include <gdk/x11/gdkx.h>
#    endif
#    ifdef GDK_WINDOWING_WAYLAND
#        include <gdk/wayland/gdkwayland.h>
#    endif
#else
#    ifdef GDK_WINDOWING_X11
#        include <gdk/gdkx.h>
#    endif
#    ifdef GDK_WINDOWING_WAYLAND
#        include <gdk/gdkwayland.h>
#    endif
#endif

#include <string>

std::string gtkPortalParentWindow(void* nativeWindow)
{
    if (!nativeWindow)
    {
        Log::warn("Opener: nativeWindow is null, portal parent_window will be empty");
        return {};
    }

    auto* widget = static_cast<GtkWidget*>(nativeWindow);
    GdkDisplay* display = gtk_widget_get_display(widget);
    if (!display)
    {
        Log::warn("Opener: failed to get GdkDisplay from widget");
        return {};
    }

#if GTK_MAJOR_VERSION >= 4
    Log::debug("Opener: using GTK4 API to extract parent window handle");

    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (!surface)
    {
        Log::warn("Opener: gtk_native_get_surface returned null — window may not yet be realized");
        return {};
    }

#    ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(display))
    {
        Log::debug("Opener: Wayland display detected (GTK4), calling gdk_wayland_toplevel_export_handle");

        struct ExportData
        {
            std::string handle;
            bool done = false;
        } data;

        gboolean const exported = gdk_wayland_toplevel_export_handle(
            GDK_TOPLEVEL(surface),
            [](GdkToplevel* /*toplevel*/, char const* handle, gpointer ptr)
            {
                auto* d = static_cast<ExportData*>(ptr);
                d->handle = std::string{"wayland:"} + (handle ? handle : "");
                d->done = true;
                Log::debug("Opener: Wayland export handle callback fired, handle='{}'", d->handle);
            },
            &data,
            nullptr);

        if (!exported)
        {
            Log::warn("Opener: gdk_wayland_toplevel_export_handle returned FALSE");
            return {};
        }

        // Spin the GLib default main context until the compositor sends back the handle event.
        // We are on the GTK main thread so this is safe; nested iterations are supported by GLib.
        int iterations = 0;
        while (!data.done)
        {
            g_main_context_iteration(nullptr, TRUE);
            if (++iterations > 100)
            {
                Log::warn("Opener: timed out waiting for Wayland handle event after {} GLib iterations", iterations);
                return {};
            }
        }

        Log::info("Opener: Wayland parent window handle resolved in {} iteration(s): '{}'", iterations, data.handle);
        return data.handle;
    }
#    endif // GDK_WINDOWING_WAYLAND

#    ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(display))
    {
        guint32 const xid = gdk_x11_surface_get_xid(surface);
        auto result = "x11:" + std::to_string(xid);
        Log::info("Opener: X11 parent window handle: '{}'", result);
        return result;
    }
#    endif // GDK_WINDOWING_X11

    Log::warn("Opener: GTK4 — display is neither Wayland nor X11, parent_window will be empty");

#else // GTK_MAJOR_VERSION < 4
    Log::debug("Opener: using GTK3 API to extract parent window handle");

    GdkWindow* gdkWindow = gtk_widget_get_window(widget);
    if (!gdkWindow)
    {
        Log::warn("Opener: gtk_widget_get_window returned null (window not yet realized?)");
        return {};
    }

#    ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(display))
    {
        Log::debug("Opener: Wayland display detected (GTK3), calling gdk_wayland_window_export_handle");

        struct ExportData
        {
            std::string handle;
            bool done = false;
        } data;

        gboolean const exported = gdk_wayland_window_export_handle(
            gdkWindow,
            [](GdkWindow* /*window*/, char const* handle, gpointer ptr)
            {
                auto* d = static_cast<ExportData*>(ptr);
                d->handle = std::string{"wayland:"} + (handle ? handle : "");
                d->done = true;
                Log::debug("Opener: GTK3 Wayland export handle callback fired, handle='{}'", d->handle);
            },
            &data,
            nullptr);

        if (!exported)
        {
            Log::warn("Opener: gdk_wayland_window_export_handle returned FALSE (GTK3)");
            return {};
        }

        int iterations = 0;
        while (!data.done)
        {
            g_main_context_iteration(nullptr, TRUE);
            if (++iterations > 100)
            {
                Log::warn(
                    "Opener: GTK3 timed out waiting for Wayland handle event after {} iterations", iterations);
                return {};
            }
        }

        Log::info(
            "Opener: GTK3 Wayland parent window handle resolved in {} iteration(s): '{}'",
            iterations,
            data.handle);
        return data.handle;
    }
#    endif // GDK_WINDOWING_WAYLAND

#    ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(display))
    {
        Window const xid = gdk_x11_window_get_xid(gdkWindow);
        auto result = "x11:" + std::to_string(xid);
        Log::info("Opener: GTK3 X11 parent window handle: '{}'", result);
        return result;
    }
#    endif // GDK_WINDOWING_X11

    Log::warn("Opener: GTK3 — display is neither Wayland nor X11, parent_window will be empty");

#endif // GTK_MAJOR_VERSION

    return {};
}
