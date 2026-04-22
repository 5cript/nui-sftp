#include <backend/opener.hpp>

#include <utility/fd_guard.hpp>
#include <utility/glib_raii.hpp>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <log/log.hpp>

#include <gtk/gtk.h>

#if GTK_MAJOR_VERSION >= 4
#    include <jsc/jsc.h>
#    include <webkit/webkit.h>
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

#include <fcntl.h>

#include <map>
#include <string>

namespace
{
    constexpr char const* portalService = "org.freedesktop.portal.Desktop";
    constexpr char const* portalObjectPath = "/org/freedesktop/portal/desktop";
    constexpr char const* openUriInterface = "org.freedesktop.portal.OpenURI";
    constexpr char const* documentsService = "org.freedesktop.portal.Documents";
    constexpr char const* documentsObjectPath = "/org/freedesktop/portal/documents";
    constexpr char const* documentsInterface = "org.freedesktop.portal.Documents";

    /**
     *  @brief True when running inside a flatpak sandbox. xdg-desktop-portal routes fd-based
     *         OpenFile calls through the Documents portal only in this case, so we include the
     *         Documents portal probe in the capability decision only for sandboxed runs.
     */
    bool runningInFlatpak()
    {
        std::error_code ec;
        return std::filesystem::exists("/.flatpak-info", ec) && !ec;
    }

    //---------------------------------------------------------------------------------------------------------------------
    // Extract the XDG portal parent_window identifier from the GtkWidget* native window.
    // On Wayland this uses zxdg_exporter_v2 (via GDK's export_handle API) to get a stable
    // exportable handle. The handle is then cached; the export is kept alive so it remains valid.
    // On X11 we just read the XID, which is always available synchronously.
    //
    // Returns "" on failure (portal will still try to show the dialog, just without a parent).
    //---------------------------------------------------------------------------------------------------------------------
    std::string extractParentWindowHandle(void* nativeWindow)
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
}

namespace
{
    /**
     *  @brief Invoke a fd-taking portal method (OpenURI.OpenFile / OpenURI.OpenDirectory).
     *         Takes ownership of @p fd -- the FdGuard closes it on every path. Pass
     *         @p withAsk = false when the method takes no "ask" option (OpenDirectory).
     */
    std::expected<void, std::string> callPortalFdMethod(
        GDBusConnection* connection,
        char const* method,
        std::string const& parentWindow,
        Utility::FdGuard fd,
        bool withAsk,
        bool askValue)
    {
        Utility::GObjectPtr<GUnixFDList> fdList{g_unix_fd_list_new()};
        GError* rawErr = nullptr;
        const gint fdHandle = g_unix_fd_list_append(fdList.get(), fd.get(), &rawErr);
        // GUnixFDList dup'd the fd on append; our copy is closed by FdGuard at scope exit.
        if (fdHandle < 0)
            return std::unexpected{Utility::consumeGError(rawErr, "g_unix_fd_list_append failed")};

        GVariantBuilder optsBuilder;
        g_variant_builder_init(&optsBuilder, G_VARIANT_TYPE("a{sv}"));
        if (withAsk)
            g_variant_builder_add(&optsBuilder, "{sv}", "ask", g_variant_new_boolean(askValue));

        GVariant* const params = g_variant_new(
            "(sh@a{sv})", parentWindow.c_str(), fdHandle, g_variant_builder_end(&optsBuilder));

        rawErr = nullptr;
        Utility::GVariantPtr result{g_dbus_connection_call_with_unix_fd_list_sync(
            connection,
            portalService,
            portalObjectPath,
            openUriInterface,
            method,
            params, // floating; sunk by the call
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            /*timeout_msec*/ 30000,
            fdList.get(),
            /*out_fd_list*/ nullptr,
            /*cancellable*/ nullptr,
            &rawErr)};

        if (!result)
            return std::unexpected{Utility::consumeGError(rawErr, "portal call failed")};

        gchar const* handlePath = nullptr;
        g_variant_get(result.get(), "(&o)", &handlePath);
        Log::info("Opener: portal.{} succeeded, request handle='{}'", method, handlePath ? handlePath : "?");
        return {};
    }

    /**
     *  @brief Fetch a uint32 property from a portal interface via org.freedesktop.DBus.Properties.
     *         Returns 0 when the property is unreachable (interface absent, service not running,
     *         call error) -- capabilities() treats 0 as "not present".
     */
    std::uint32_t
    probePortalVersion(GDBusConnection* connection, char const* service, char const* objectPath, char const* iface)
    {
        if (!connection)
            return 0;

        GError* rawErr = nullptr;
        Utility::GVariantPtr result{g_dbus_connection_call_sync(
            connection,
            service,
            objectPath,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", iface, "version"),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE,
            /*timeout_msec*/ 2000,
            /*cancellable*/ nullptr,
            &rawErr)};

        if (!result)
        {
            Log::warn(
                "Opener::capabilities: {} version probe failed: {}",
                iface,
                Utility::consumeGError(rawErr, "(no error object)"));
            return 0;
        }

        GVariant* innerRaw = nullptr;
        g_variant_get(result.get(), "(v)", &innerRaw);
        Utility::GVariantPtr inner{innerRaw};
        if (inner && g_variant_is_of_type(inner.get(), G_VARIANT_TYPE_UINT32))
            return g_variant_get_uint32(inner.get());
        return 0;
    }

    /**
     *  @brief Ask the system's default handler (via the shared MIME DB / GAppInfo) to open
     *         @p path. Used for directories in openInFileManager, where the portal's
     *         OpenDirectory method is spec'd to open the fd's *parent* -- not useful when
     *         the user already picked the directory itself.
     */
    std::expected<void, std::string> launchDefaultForPath(std::filesystem::path const& path)
    {
        GError* uriErrRaw = nullptr;
        Utility::GcharPtr uri{g_filename_to_uri(path.c_str(), nullptr, &uriErrRaw)};
        if (!uri)
            return std::unexpected{Utility::consumeGError(uriErrRaw, "g_filename_to_uri failed")};

        GError* launchErrRaw = nullptr;
        const gboolean launched = g_app_info_launch_default_for_uri(uri.get(), nullptr, &launchErrRaw);
        Log::info("Opener: launching file manager for URI '{}'", uri.get());
        if (!launched)
            return std::unexpected{Utility::consumeGError(launchErrRaw, "launch failed")};
        return {};
    }
}

struct Opener::Implementation
{
    std::string parentWindow; // "wayland:HANDLE" | "x11:XID" | ""
    Utility::GObjectPtr<GDBusConnection> connection;

    explicit Implementation(void* nativeWindow)
        : parentWindow{extractParentWindowHandle(nativeWindow)}
    {
        // Use GLib's GDBus rather than sd-bus: Ubuntu 24.04's xdg-dbus-proxy mangles the AUTH
        // handshake that sd-bus's sd_bus_open_user() performs (one-shot "AUTH EXTERNAL <uid>"),
        // but accepts GDBus's multi-step flow unchanged. The shared session-bus connection
        // returned by g_bus_get_sync() is the same one GTK/WebKit are already using, so this
        // doesn't open a second socket.
        GError* rawErr = nullptr;
        connection.reset(g_bus_get_sync(G_BUS_TYPE_SESSION, /*cancellable*/ nullptr, &rawErr));
        if (!connection)
        {
            Log::warn(
                "Opener: could not connect to session bus ({}). "
                "Opening files via xdg-desktop-portal will be unavailable.",
                Utility::consumeGError(rawErr, "(no error object)"));
        }
        Log::info("Opener: initialized with parentWindow='{}'", parentWindow);
    }
};

Opener::Opener(void* nativeWindow)
    : impl_{std::make_unique<Implementation>(nativeWindow)}
{}

Opener::~Opener() = default;

Opener::Opener(Opener&&) noexcept = default;

Opener& Opener::operator=(Opener&&) noexcept = default;

std::expected<void, std::string> Opener::openFile(std::filesystem::path const& path, bool openWith)
{
    Log::info("Opener: openFile path='{}' openWith={} parentWindow='{}'", path.string(), openWith, impl_->parentWindow);

    if (!impl_->connection)
    {
        constexpr auto const* msg =
            "Session bus is unavailable; cannot open files via xdg-desktop-portal. "
            "This typically happens when running as root or without a user D-Bus session.";
        Log::error("Opener: {}", msg);
        return std::unexpected{std::string{msg}};
    }

    Utility::FdGuard fd{::open(path.c_str(), O_RDONLY)};
    if (!fd.valid())
    {
        Log::error("Opener: failed to open fd for '{}': {}", path.string(), std::strerror(errno));
        return std::unexpected{std::string{"Failed to open file: "} + std::strerror(errno)};
    }

    auto result = callPortalFdMethod(
        impl_->connection.get(), "OpenFile", impl_->parentWindow, std::move(fd), /*withAsk*/ true, openWith);
    if (!result)
        Log::error("Opener: OpenURI.OpenFile portal call failed: {}", result.error());
    return result;
}

SharedData::OpenerCapabilities Opener::capabilities() const
{
    SharedData::OpenerCapabilities caps;

    if (!impl_->connection)
    {
        caps.canOpenFile = false;
        caps.canOpenInFileManager = false;
        caps.reason = "D-Bus session bus is unavailable.";
        return caps;
    }

    // 1) OpenURI interface must exist on xdg-desktop-portal.
    const std::uint32_t openUriVersion = probePortalVersion(
        impl_->connection.get(), portalService, portalObjectPath, openUriInterface);
    Log::info("Opener::capabilities: OpenURI version={}", openUriVersion);
    if (openUriVersion == 0)
    {
        caps.canOpenFile = false;
        caps.canOpenInFileManager = false;
        caps.reason = "xdg-desktop-portal OpenURI interface is unavailable.";
        return caps;
    }

    // 2) Inside flatpak, OpenFile(fd) goes through the Documents portal; if it's not reachable
    //    the call will fail even though OpenURI is present. Outside flatpak the portal can open
    //    the fd directly, so skip the probe.
    if (runningInFlatpak())
    {
        const std::uint32_t documentsVersion = probePortalVersion(
            impl_->connection.get(), documentsService, documentsObjectPath, documentsInterface);
        Log::info("Opener::capabilities: Documents portal version={}", documentsVersion);
        if (documentsVersion == 0)
        {
            caps.canOpenFile = false;
            caps.reason = "xdg-document-portal is unavailable; "
                          "file-descriptor-based opening is not possible in this flatpak.";
            // openInFileManager for directories uses g_app_info_launch_default_for_uri, which
            // does not require the Documents portal, so leave canOpenInFileManager untouched.
        }
    }

    return caps;
}

std::expected<void, std::string> Opener::openInFileManager(std::filesystem::path const& path)
{
    Log::info(
        "Opener: openInFileManager path='{}' parentWindow='{}'", path.string(), impl_->parentWindow);

    if (!impl_->connection)
    {
        constexpr auto const* msg =
            "Session bus is unavailable; cannot open file manager via xdg-desktop-portal. "
            "This typically happens when running as root or without a user D-Bus session.";
        Log::error("Opener: {}", msg);
        return std::unexpected{std::string{msg}};
    }

    std::error_code directoryCheckEc;
    const bool isDirectory = std::filesystem::is_directory(path, directoryCheckEc);

    if (isDirectory)
    {
        auto result = launchDefaultForPath(path);
        if (!result)
            Log::error("Opener: g_app_info_launch_default_for_uri failed for '{}': {}", path.string(), result.error());
        return result;
    }

    // File (or nonexistent — portal will reject that): OpenDirectory opens the
    // file's parent directory in the file manager. Most file managers highlight
    // the file, which is the common "reveal in file manager" behavior.
    Utility::FdGuard fd{::open(path.c_str(), O_RDONLY)};
    if (!fd.valid())
    {
        Log::error("Opener: failed to open fd for '{}': {}", path.string(), std::strerror(errno));
        return std::unexpected{std::string{"Failed to open file: "} + std::strerror(errno)};
    }

    auto result = callPortalFdMethod(
        impl_->connection.get(), "OpenDirectory", impl_->parentWindow, std::move(fd), /*withAsk*/ false, false);
    if (!result)
        Log::error("Opener: OpenURI.OpenDirectory portal call failed: {}", result.error());
    return result;
}
