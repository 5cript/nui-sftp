#include <backend/main.hpp>
#include <backend/rpc_helper.hpp>

#ifdef _WIN32
#    include <backend/windows/main_windows.hpp>
#elif defined(NUI_ENGINE_CEF)
#    include <backend/cef/main_cef.hpp>
#    include <backend/process/fork_pool.hpp>
#elif defined(__APPLE__)
#    include <backend/mac/main_mac.hpp>
#    include <backend/process/fork_pool.hpp>
#else
#    include <backend/linux/gtk_portal_parent_window.hpp>
#    include <backend/linux/main_linux.hpp>
#    include <backend/process/fork_pool.hpp>
#endif

#ifdef __linux__
#    include <glib.h>
#endif

#include <backend/browser_engine.hpp>
#include <backend/inspector_environment.hpp>
#include <paths/local_path_segment.hpp>
#include <paths/native.hpp>
#include <backend/process/process_store.hpp>
#include <backend/program_options.hpp>
#include <constants/persistence.hpp>
#include <utility/permissions.hpp>
#include <utility/resources.hpp>
#include <version.hpp>
#include <nui/backend/filesystem/special_paths.hpp>
#include <nui/backend/webkit_gtk_rendering.hpp>

#include <nui/core.hpp>
#include <nui/rpc.hpp>
#include <nui/window.hpp>
#ifdef NUI_ENGINE_CEF
#    include <nui/cef/client_hooks.hpp>
#endif
#include <roar/mime_type.hpp>
#include <efsw/efsw.hpp>
#include <log/log.hpp>
#include <libssh/libsshpp.hpp>
#include <boost/dll/runtime_symbol_info.hpp>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <algorithm>
#include <ranges>
#include <span>
#include <optional>
#include <unordered_map>
#include <cstdlib>

#ifndef _WIN32
#    include <signal.h>
#    include <unistd.h>
#endif

#ifndef NDEBUG
#    include <build_environment.hpp>
#endif

using namespace std::string_literals;
using namespace std::chrono_literals;
using namespace Nui;

#ifndef _WIN32
volatile sig_atomic_t sigchld[10] = {0};
#endif

namespace
{
    /**
     * @brief Parent window of the xdg-desktop-portal dialogs of the opener, empty where there is none.
     */
    std::string portalParentWindow([[maybe_unused]] Nui::Window& window)
    {
#if defined(NUI_ENGINE_CEF)
        return Nui::Cef::portalParentWindow(window);
#elif defined(__linux__)
        return gtkPortalParentWindow(window.getNativeWindow());
#else
        return {};
#endif
    }

    /**
     * @brief Browser switches of the window. Debug builds make WASM out-of-bounds accesses throw a RuntimeError,
     *        without a crash handler in the renderer they crash it silently.
     */
    std::optional<std::string> browserArguments()
    {
#ifndef NDEBUG
        return "--js-flags=--wasm-enforce-bounds-checks"s;
#else
        return std::nullopt;
#endif
    }

    auto makeResponse(int code, std::string const& reason, std::string body, std::string const& mimeType = ""s)
    {
        std::unordered_multimap<std::string, std::string> headers = {
            {"Content-Type"s, mimeType.empty() ? "text/plain" : mimeType},
            // Do not forget to allow CORS
            {"Access-Control-Allow-Origin"s, "*"s},
        };

        if (!body.empty())
            headers.emplace("Content-Length"s, std::to_string(body.size()));

        return CustomSchemeResponse{
            .statusCode = code,
            .reasonPhrase = reason,
            .headers = std::move(headers),
            .body = std::move(body),
        };
    };

    auto makeFileResponse(int code, std::string const& reason, std::filesystem::path file, std::string const& mimeType)
    {
        std::unordered_multimap<std::string, std::string> headers = {
            {"Content-Type"s, mimeType.empty() ? "application/octet-stream"s : mimeType},
            {"Access-Control-Allow-Origin"s, "*"s},
        };
        return CustomSchemeResponse{
            .statusCode = code,
            .reasonPhrase = reason,
            .headers = std::move(headers),
            .bodyFile = std::move(file),
        };
    };

    CustomScheme createFolderMapping(std::filesystem::path const& programDir, std::string const& schemeName)
    {
        return CustomScheme{
            .scheme = schemeName,
            .allowedOrigins = {"*"s},
            .onRequest =
                [programDir, schemeName, resourceDir = resourceRoot(programDir)](CustomSchemeRequest const& request)
            {
                // make path relative to / to avoid directory traversal
                const auto url = request.parseUrl();
                if (!url)
                {
                    Log::error("Failed to parse url: '{}'", request.uri);
                    return makeResponse(400, "Bad Request", "Bad Request");
                }

                const auto pathString = url->pathAsString();
                Log::debug("Request for {}", pathString);

                if (!isCanonical(pathString))
                {
                    Log::error("Path is not canonical: '{}'", pathString);
                    return makeResponse(404, "Not Found", "Not Found");
                }

                const auto fileOpt = mapUrlToFile(resourceDir, pathString);
                if (!fileOpt)
                {
                    Log::error("No mapping for url: '{}'", pathString);
                    return makeResponse(404, "Not Found", "Not Found");
                }
                const auto file = *fileOpt;

                //                 if (
                //                     !pointsToWithinDir(resourceDir, file) &&
                //                     !pointsToWithinDir(Nui::resolvePath("%state_home2%"), file)
                // #ifndef NDEBUG
                //                     && !pointsToWithinDir(SOURCE_DIR, file)
                // #endif
                //                 )
                //                 {
                //                     Log::error("Path points outside of program directory: '{}'", file.string());
                //                     return makeResponse(404, "Not Found", "Not Found");
                //                 }

                // Check if file exists and return 404 if not
                if (!std::filesystem::exists(file))
                {
                    Log::error("File not found: '{}'", Paths::nativeToUtf8(file));
                    return CustomSchemeResponse{
                        .statusCode = 404,
                        .reasonPhrase = "Not Found",
                        .headers =
                            {
                                {"Content-Type"s, "text/plain"s},
                                // Do not forget to allow CORS
                                {"Access-Control-Allow-Origin"s, "*"s},
                            },
                        .body = fmt::format("Not Found: {}", Paths::nativeToUtf8(file)),
                    };
                }

                Log::debug("Serving file: '{}'", Paths::nativeToUtf8(file));

                // Stream the file body via bodyFile, which avoids loading the whole file into memory
                // (matters for large assets like DWARF blobs). Empty files yield a 204.
                std::error_code sizeEc;
                const auto fileSize = std::filesystem::file_size(file, sizeEc);
                const auto code = (!sizeEc && fileSize == 0) ? 204 : 200;

                // Roar's MIME table has no entry for .wasm; emit application/wasm explicitly so that
                // WebAssembly.instantiateStreaming() accepts the response and compiles the module as
                // bytes arrive (otherwise the browser falls back to buffering the full binary first).
                const auto ext = Paths::nativeToUtf8(file.extension());
                const auto mime = (ext == ".wasm")
                    ? std::string{"application/wasm"}
                    : Roar::extensionToMime(ext).value_or("application/octet-stream");
                return makeFileResponse(code, "OK", file, mime);
            },

            // Windows: Is this secure like https (not http)? A lot of things are not allowed in http.
            .treatAsSecure = true,

            // Windows: Do urls to this custom scheme have an authority component? (For portability reasons, they
            // usually should have).
            .hasAuthorityComponent = true,

            // CEF: the handler only reads files, requests are served in parallel off the UI thread.
            .handlerThread = SchemeHandlerThread::Any,
        };
    }
}

Main::LoggerSetup::LoggerSetup(Persistence::StateHolder& stateHolder)
{
    stateHolder.load(
        [](std::optional<std::string> const&, Persistence::StateHolder& holder, Persistence::LoadWarnings const&)
        {
            auto const& state = holder.stateCache();
            Log::Logger::setupGlobalSinks(
                state.logOptions.logLevel, state.logOptions.logDirectory, state.logOptions.disableFileLogging
            );
        }
    );
}

Main::Main(ProgramOptions options, ForkPool* forkPool)
    : shuttingDown_{false}
    , devToolsEnabled_{options.enableDevTools}
    , programDir_{boost::dll::program_location().parent_path().native()}
    , stateHolder_{programDir_}
    , loggerSetup_{stateHolder_}
    , window_{
          Nui::WindowOptions{
              .title = "NuiSftp"s,
              .debug = options.enableDevTools,
              .customSchemes = {createFolderMapping(programDir_, "nui")},
              .browserArguments = browserArguments(),
              .onRpcAliveMessage = [this]() {
                  onRpcAlive();
              }
          },
      }
    , hub_{window_}
    , opener_{}
    , riskyFilePolicy_{Persistence::RiskyFilePolicy::Warn}
    , rpcFilesystem_{}
    , rpcSystem_{}
    , processes_{window_.getExecutor(), window_, hub_, forkPool}
    , prompter_{hub_}
    , sshSessionManager_{std::make_shared<SessionManager>(window_.getExecutor(), stateHolder_, window_, hub_)}
    , childSignalTimer_{window_.getExecutor()}
    , events_{hub_}
    , themeFinder_{resourceRoot(programDir_), events_}
    , platformSpecifics_{std::make_unique<PlatformSpecifics>(window_, hub_)}
{
    sshSessionManager_->addPasswordProvider(-99, &prompter_);
}
Main::~Main()
{
    shuttingDown_ = true;
    // No longer forward logs to the view:
    Log::Detail::logger.detach();
    // sshSessionManager_->stopUpdateDispatching();
    // Releases a strand job blocked on an unanswered password prompt, the window joins its pool later.
    sshSessionManager_->shutdown();
    childSignalTimer_.cancel();
}

void Main::onRpcAlive()
{
    std::call_once(
        rpcAliveOnce_,
        [this]()
        {
            registerRpc();
        }
    );
}

void Main::registerRpc()
{
    hub_.enableFetch();
    hub_.enableTimer();
    hub_.enableWindowFunctions();
    hub_.enableEnvironmentVariables();
    hub_.enableThrottle();
    hub_.enableFileDialogs();

    Log::setupBackendRpcHub(&hub_);
    prompter_.registerRpc();
    stateHolder_.registerRpc(hub_);
    processes_.registerRpc(window_, hub_);
    sshSessionManager_->registerRpc();
    registerInitialWarningGetter();
    registerVersionGetter();
    registerDevToolsGetter();
    registerCommandStore();

    defaultPlacesProvider_ = std::make_unique<NuiFileExplorer::DefaultPlacesProvider>(hub_);
#ifdef _WIN32
    windowsDrivesProvider_ = std::make_unique<NuiFileExplorer::WindowsDrivesProvider>(hub_);
#endif

    stateHolder_.load(
        [this](
            std::optional<std::string> const& error,
            Persistence::StateHolder& holder,
            Persistence::LoadWarnings const& warnings
        )
        {
            if (!warnings.empty())
            {
                Log::warn("Warnings loading state: {}", nlohmann::json(warnings).dump());
                initialPersistenceLoadWarnings_ = warnings;
            }

            const auto riskyFilePolicyOf = [](Persistence::State const& state)
            {
                return state.localFilesystemOptions.openRiskyFiles.value_or(Persistence::RiskyFilePolicy::Warn);
            };
            riskyFilePolicy_.store(riskyFilePolicyOf(holder.stateCache()));
            stateHolder_.onSaved(
                [this, riskyFilePolicyOf](Persistence::State const& state)
                {
                    riskyFilePolicy_.store(riskyFilePolicyOf(state));
                    sshSessionManager_->fileTrackingDefaults(state.fileTrackingOptions);
                }
            );

            opener_ = std::make_unique<Opener>(portalParentWindow(window_));
            rpcSystem_ = std::make_unique<RpcSystem>(window_.getExecutor(), window_, hub_);
            const auto temporaryDownloadsDirectory =
                Nui::resolvePath(holder.stateCache().localFilesystemOptions.temporaryDownloadsDirectory.value_or(
                    Persistence::LocalFilesystemOptions{}.temporaryDownloadsDirectory.value()
                ));
            sshSessionManager_->startFileTracking(temporaryDownloadsDirectory);
            sshSessionManager_->fileTrackingDefaults(holder.stateCache().fileTrackingOptions);

            if (error)
            {
                Log::error("Setting up rpc filesystem with full lockdown due to state load error: {}", *error);

                rpcFilesystem_ = std::make_unique<RpcFilesystem>(
                    window_.getExecutor(),
                    window_,
                    hub_,
                    // prevent all:
                    Persistence::LocalFilesystemOptions{
                        .preventDeletion = true,
                        .preventRename = true,
                        .preventCreateFile = true,
                        .preventCreateDirectory = true,
                    },
                    *opener_
                );
                sshSessionManager_->preventLocalDeletion(true);
                return;
            }

            sshSessionManager_->preventLocalDeletion(holder.stateCache().localFilesystemOptions.preventDeletion);
            rpcFilesystem_ = std::make_unique<RpcFilesystem>(
                window_.getExecutor(),
                window_,
                hub_,
                holder.stateCache().localFilesystemOptions,
                *opener_,
                [this]()
                {
                    return riskyFilePolicy_.load();
                }
            );

            hub_.markRpcAsInitialized();
        }
    );
}

void Main::show()
{
    window_.setSize(1900, 1000, Nui::WebViewHint::WEBVIEW_HINT_NONE);
    // window_.centerOnPrimaryDisplay();
    // window_.openDevTools();
    window_.navigate("nui://app.example/index.html");
    window_.setConsoleOutput(false);
    window_.run();
}

void Main::registerCommandStore()
{
    // User data directory: the program directory is read-only in installed packages.
    const auto databaseFile = Nui::resolvePath(Constants::commandStorePath);
    std::error_code directoryError;
    std::filesystem::create_directories(databaseFile.parent_path(), directoryError);

    auto store = CommandStore::Store::open(window_.getExecutor(), databaseFile);
    if (!store)
    {
        Log::error("Failed to open the command store, history and snippets are unavailable: {}", store.error().message);
        commandStoreRpc_ = std::make_unique<CommandStore::UnavailableStoreRpc>(
            window_.getExecutor(), window_, hub_, store.error().message
        );
        return;
    }

    commandStore_.emplace(std::move(*store));
    commandStoreRpc_ = std::make_unique<CommandStore::StoreRpc>(window_.getExecutor(), window_, hub_, *commandStore_);
}

void Main::registerInitialWarningGetter()
{
    hub_.registerFunction(
        "Main::getInitialPersistenceLoadWarning",
        [this](std::string responseId)
        {
            Log::debug("Received request for initial persistence load warning.");

            const bool runningAsRoot =
#ifndef _WIN32
                ::geteuid() == 0;
#else
                false;
#endif

            RpcHelper::callRemoteSafely(
                hub_,
                responseId,
                nlohmann::json{
                    {"warnings", initialPersistenceLoadWarnings_},
                    {"isRoot", runningAsRoot},
                }
            );
        }
    );
}

void Main::registerVersionGetter()
{
    hub_.registerFunction(
        "Main::getVersion",
        [this](std::string responseId)
        {
            RpcHelper::callRemoteSafely(
                hub_,
                responseId,
                nlohmann::json{
                    {"version", VERSION},
                    {"gitTag", GIT_TAG},
                    {"gitBranch", GIT_BRANCH},
                    {"localPlatform",
                     Paths::compiledLocalPlatform() == Paths::LocalPlatform::Windows ? "windows" : "posix"},
                    {"browserEngine", browserEngineDescription()},
                }
            );
        }
    );
}

void Main::registerDevToolsGetter()
{
    hub_.registerFunction(
        "Main::getDevToolsEnabled",
        [this](std::string responseId)
        {
            RpcHelper::callRemoteSafely(hub_, responseId, nlohmann::json{{"enabled", devToolsEnabled_}});
        }
    );
}

void Main::startChildSignalTimer()
{
    if (shuttingDown_)
        return;

#ifndef _WIN32
    childSignalTimer_.expires_after(200ms);
    childSignalTimer_.async_wait(
        [this](boost::system::error_code const& ec)
        {
            if (ec)
                return;

            for (auto& i : sigchld)
            {
                if (i > 0)
                {
                    window_.runInJavascriptThread(
                        [i, this]()
                        {
                            processes_.notifyChildExit(hub_, i);
                        }
                    );
                    i = 0;
                }
            }

            startChildSignalTimer();
        }
    );
#endif
}

int main(int const argc, char const* const* argv)
{
    // Reading the umask changes it for a moment on macOS, so it is read before any thread creates files.
    static_cast<void>(Utility::processUmask());
    // Before any thread exists, it uses setenv.
    const auto webKitGtkRendering = Nui::configureWebKitGtkRendering();
#ifdef _WIN32
    // A GUI program has no console; --console, the help and parse errors write to the one it was started from.
    const auto arguments = std::span{argv, static_cast<std::size_t>(argc)} | std::views::drop(1);
    const bool consoleRequested = std::ranges::any_of(
        arguments,
        [](std::string_view argument)
        {
            return argument == "--console";
        }
    );
    const auto console = consoleRequested ? attachParentConsole() : ConsoleAttachment{};
#endif
    auto options = parseProgramOptions(argc, argv);
    if (!options)
    {
#ifdef _WIN32
        const auto exitConsole = consoleRequested ? ConsoleAttachment{} : attachParentConsole();
#endif
        (options.error().isError ? std::cerr : std::cout) << options.error().message << '\n';
        return options.error().isError ? 1 : 0;
    }

    if (!options->enableDevTools)
        removeRemoteInspectorEnvironment();

#ifndef _WIN32

    boost::asio::thread_pool ioPool{4};
    ForkPool forkPool;
    forkPool.start(ioPool.get_executor(), nullptr);
    ForkPool* forkPoolPtr = &forkPool;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wc99-designator"
    struct sigaction sa{
        .sa_sigaction =
            +[](int, siginfo_t* info, void*)
        {
            const pid_t pid = info->si_pid;
            if (pid > 0)
            {
                for (auto& i : sigchld)
                {
                    if (i == 0)
                    {
                        i = pid;
                        break;
                    }
                }
            }
        },
        .sa_mask = {},
        .sa_flags = SA_SIGINFO,
#    ifdef __linux__
        .sa_restorer = nullptr,
#    endif
    };

    sigaction(SIGCHLD, &sa, nullptr);
#    pragma clang diagnostic pop
#else
    ForkPool* forkPoolPtr = nullptr;
#endif

#ifdef __linux__
    // Names the X11 WM_CLASS and the Wayland app id: GTK 4 does not take it from argv like GTK 3, and Nui passes it to
    // Chromium. In a flatpak WebKit has to use the app id, the portal refuses to spawn its sandboxed web process otherwise.
    const char* const flatpakApplicationId = std::getenv("FLATPAK_ID");
    g_set_prgname(flatpakApplicationId != nullptr && *flatpakApplicationId != '\0' ? flatpakApplicationId : "nui-sftp");
#endif

    ssh_init();

    {
        Main m{std::move(*options), forkPoolPtr};
        if (webKitGtkRendering.decision != Nui::DmabufRendererDecision::NotApplicable)
            Log::info("WebKitGTK DMA-BUF renderer: {}", webKitGtkRendering.reason);
        m.startChildSignalTimer();
        m.show();
    }

    ssh_finalize();
}
