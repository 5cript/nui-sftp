#include <backend/opener.hpp>

#include <sdbus-c++/sdbus-c++.h>
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

struct Opener::Implementation
{
    std::string parentWindow; // "wayland:HANDLE" | "x11:XID" | ""
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IProxy> openUriProxy;

    explicit Implementation(void* nativeWindow)
        : parentWindow{extractParentWindowHandle(nativeWindow)}
    {
        try
        {
            connection = sdbus::createSessionBusConnection();
            openUriProxy = sdbus::createProxy(
                *connection, sdbus::ServiceName{portalService}, sdbus::ObjectPath{portalObjectPath});
        }
        catch (sdbus::Error const& err)
        {
            Log::warn(
                "Opener: could not connect to session bus ({}). "
                "Opening files via xdg-desktop-portal will be unavailable.",
                err.getMessage());
        }
        catch (std::exception const& err)
        {
            Log::warn(
                "Opener: unexpected error connecting to session bus ({}). "
                "Opening files via xdg-desktop-portal will be unavailable.",
                err.what());
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

    if (!impl_->openUriProxy)
    {
        constexpr auto const* msg =
            "Session bus is unavailable; cannot open files via xdg-desktop-portal. "
            "This typically happens when running as root or without a user D-Bus session.";
        Log::error("Opener: {}", msg);
        return std::unexpected{std::string{msg}};
    }

    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
    {
        Log::error("Opener: failed to open fd for '{}': {}", path.string(), std::strerror(errno));
        return std::unexpected{std::string{"Failed to open file: "} + std::strerror(errno)};
    }

    std::map<std::string, sdbus::Variant> options;
    options.emplace("ask", sdbus::Variant{openWith});

    try
    {
        sdbus::ObjectPath handle;
        impl_->openUriProxy->callMethod("OpenFile")
            .onInterface(openUriInterface)
            .withArguments(impl_->parentWindow, sdbus::UnixFd{fd}, options)
            .storeResultsTo(handle);
        Log::info("Opener: OpenURI.OpenFile portal call succeeded, request handle='{}'", static_cast<std::string>(handle));
    }
    catch (sdbus::Error const& err)
    {
        Log::error("Opener: OpenURI.OpenFile portal call failed: {}", err.getMessage());
        return std::unexpected{err.getMessage()};
    }

    return {};
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

    // 1) OpenURI interface must exist on xdg-desktop-portal. Query its `version` property --
    //    if the interface isn't implemented the getter throws.
    bool openUriPresent = false;
    try
    {
        auto probeProxy = sdbus::createProxy(
            *impl_->connection, sdbus::ServiceName{portalService}, sdbus::ObjectPath{portalObjectPath});
        sdbus::Variant version = probeProxy->getProperty("version").onInterface(openUriInterface);
        const auto v = version.get<uint32_t>();
        openUriPresent = (v > 0);
        Log::info("Opener::capabilities: OpenURI version={}", v);
    }
    catch (sdbus::Error const& err)
    {
        Log::warn("Opener::capabilities: OpenURI probe failed: {}", err.getMessage());
    }
    catch (std::exception const& err)
    {
        Log::warn("Opener::capabilities: OpenURI probe unexpected error: {}", err.what());
    }

    if (!openUriPresent)
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
        bool documentsPresent = false;
        try
        {
            auto docsProxy = sdbus::createProxy(
                *impl_->connection,
                sdbus::ServiceName{documentsService},
                sdbus::ObjectPath{documentsObjectPath});
            sdbus::Variant version = docsProxy->getProperty("version").onInterface(documentsInterface);
            const auto v = version.get<uint32_t>();
            documentsPresent = (v > 0);
            Log::info("Opener::capabilities: Documents portal version={}", v);
        }
        catch (sdbus::Error const& err)
        {
            Log::warn("Opener::capabilities: Documents portal probe failed: {}", err.getMessage());
        }
        catch (std::exception const& err)
        {
            Log::warn("Opener::capabilities: Documents portal probe unexpected error: {}", err.what());
        }

        if (!documentsPresent)
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

    if (!impl_->openUriProxy)
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
        // Directories: bypass the portal. The portal's OpenURI method is unreliable for
        // file:// URIs (some backends reject it, others silently drop it), and OpenDirectory
        // is spec'd to open the *parent* of its fd — not useful when the user picked a dir.
        // g_app_info_launch_default_for_uri talks directly to the user's preferred file
        // manager via the shared MIME DB, which is exactly what we want here.
        GError* uriError = nullptr;
        gchar* const uri = g_filename_to_uri(path.c_str(), nullptr, &uriError);
        if (!uri)
        {
            const std::string msg = uriError ? uriError->message : "g_filename_to_uri failed";
            Log::error("Opener: failed to build file URI for '{}': {}", path.string(), msg);
            if (uriError)
                g_error_free(uriError);
            return std::unexpected{msg};
        }

        GError* launchError = nullptr;
        const gboolean launched = g_app_info_launch_default_for_uri(uri, nullptr, &launchError);
        Log::info("Opener: launching file manager for URI '{}'", uri);
        g_free(uri);
        if (!launched)
        {
            const std::string msg = launchError ? launchError->message : "launch failed";
            Log::error("Opener: g_app_info_launch_default_for_uri failed: {}", msg);
            if (launchError)
                g_error_free(launchError);
            return std::unexpected{msg};
        }

        return {};
    }

    // File (or nonexistent — portal will reject that): OpenDirectory opens the
    // file's parent directory in the file manager. Most file managers highlight
    // the file, which is the common "reveal in file manager" behavior.
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
    {
        Log::error("Opener: failed to open fd for '{}': {}", path.string(), std::strerror(errno));
        return std::unexpected{std::string{"Failed to open file: "} + std::strerror(errno)};
    }

    const std::map<std::string, sdbus::Variant> options;
    try
    {
        sdbus::ObjectPath handle;
        impl_->openUriProxy->callMethod("OpenDirectory")
            .onInterface(openUriInterface)
            .withArguments(impl_->parentWindow, sdbus::UnixFd{fd}, options)
            .storeResultsTo(handle);
        Log::info(
            "Opener: OpenURI.OpenDirectory portal call succeeded, request handle='{}'",
            static_cast<std::string>(handle));
    }
    catch (sdbus::Error const& err)
    {
        Log::error("Opener: OpenURI.OpenDirectory portal call failed: {}", err.getMessage());
        return std::unexpected{err.getMessage()};
    }

    return {};
}
