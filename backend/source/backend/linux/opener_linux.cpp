#include <backend/opener.hpp>
#include <backend/opener_risk.hpp>
#include <paths/native.hpp>
#include <backend/process/environment.hpp>
#include <utility/localized_message.hpp>

#include <utility/fd_guard.hpp>
#include <utility/glib_raii.hpp>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <log/log.hpp>

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
     *  @brief Launch context that starts programs with the host environment instead of the AppImage one.
     */
    Utility::GObjectPtr<GAppLaunchContext> hostLaunchContext()
    {
        Utility::GObjectPtr<GAppLaunchContext> context{g_app_launch_context_new()};
        Environment hostEnvironment;
        hostEnvironment.loadFromCurrent();
        auto const& variables = hostEnvironment.environment();

        const Utility::GStrvPtr currentNames{g_listenv()};
        for (auto** name = currentNames.get(); *name != nullptr; ++name)
        {
            if (!variables.contains(*name))
                g_app_launch_context_unsetenv(context.get(), *name);
        }
        for (auto const& [name, value] : variables)
        {
            if (!name.empty())
                g_app_launch_context_setenv(context.get(), name.c_str(), value.c_str());
        }
        return context;
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

        auto context = hostLaunchContext();
        GError* launchErrRaw = nullptr;
        const gboolean launched = g_app_info_launch_default_for_uri(uri.get(), context.get(), &launchErrRaw);
        Log::info("Opener: launching file manager for URI '{}'", uri.get());
        if (!launched)
            return std::unexpected{Utility::consumeGError(launchErrRaw, "launch failed")};
        return {};
    }
}

struct Opener::Implementation
{
    /// "wayland:HANDLE", "x11:XID" or empty.
    std::string parentWindow;
    Utility::GObjectPtr<GDBusConnection> connection;

    explicit Implementation(std::string portalParentWindow)
        : parentWindow{std::move(portalParentWindow)}
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

Opener::Opener(std::string portalParentWindow)
    : impl_{std::make_unique<Implementation>(std::move(portalParentWindow))}
{}

Opener::~Opener() = default;

Opener::Opener(Opener&&) noexcept = default;

Opener& Opener::operator=(Opener&&) noexcept = default;

std::optional<std::string> Opener::riskOf(std::filesystem::path const& path) const
{
    return OpenerDetail::linuxRiskOf(path);
}

std::expected<void, std::string> Opener::openFile(std::filesystem::path const& path, bool openWith)
{
    Log::info(
        "Opener: openFile path='{}' openWith={} parentWindow='{}'",
        Paths::nativeToUtf8(path),
        openWith,
        impl_->parentWindow
    );

    if (!impl_->connection)
    {
        Log::error("Opener: session bus is unavailable, cannot open files via xdg-desktop-portal.");
        return std::unexpected{std::string{"backend.opener.sessionBusUnavailableOpenFile"}};
    }

    Utility::FdGuard fd{::open(path.c_str(), O_RDONLY)};
    if (!fd.valid())
    {
        Log::error("Opener: failed to open fd for '{}': {}", Paths::nativeToUtf8(path), std::strerror(errno));
        return std::unexpected{Utility::localizedMessage("backend.opener.openFileFailed", std::strerror(errno))};
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
        caps.reason = "backend.opener.sessionBusUnavailable";
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
        caps.reason = "backend.opener.openUriUnavailable";
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
            caps.reason = "backend.opener.documentPortalUnavailable";
            // openInFileManager for directories uses g_app_info_launch_default_for_uri, which
            // does not require the Documents portal, so leave canOpenInFileManager untouched.
        }
    }

    return caps;
}

std::expected<void, std::string> Opener::openInFileManager(std::filesystem::path const& path)
{
    Log::info("Opener: openInFileManager path='{}' parentWindow='{}'", Paths::nativeToUtf8(path), impl_->parentWindow);

    if (!impl_->connection)
    {
        Log::error("Opener: session bus is unavailable, cannot open the file manager via xdg-desktop-portal.");
        return std::unexpected{std::string{"backend.opener.sessionBusUnavailableFileManager"}};
    }

    std::error_code directoryCheckEc;
    const bool isDirectory = std::filesystem::is_directory(path, directoryCheckEc);

    if (isDirectory)
    {
        auto result = launchDefaultForPath(path);
        if (!result)
            Log::error(
                "Opener: g_app_info_launch_default_for_uri failed for '{}': {}",
                Paths::nativeToUtf8(path),
                result.error()
            );
        return result;
    }

    // File (or nonexistent — portal will reject that): OpenDirectory opens the
    // file's parent directory in the file manager. Most file managers highlight
    // the file, which is the common "reveal in file manager" behavior.
    Utility::FdGuard fd{::open(path.c_str(), O_RDONLY)};
    if (!fd.valid())
    {
        Log::error("Opener: failed to open fd for '{}': {}", Paths::nativeToUtf8(path), std::strerror(errno));
        return std::unexpected{Utility::localizedMessage("backend.opener.openFileFailed", std::strerror(errno))};
    }

    auto result = callPortalFdMethod(
        impl_->connection.get(), "OpenDirectory", impl_->parentWindow, std::move(fd), /*withAsk*/ false, false);
    if (!result)
        Log::error("Opener: OpenURI.OpenDirectory portal call failed: {}", result.error());
    return result;
}
