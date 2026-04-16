#include <backend/main.hpp>

#ifdef _WIN32
#    include <backend/windows/main_windows.hpp>
#else
#    include <backend/linux/main_linux.hpp>
#    include <backend/process/fork_pool.hpp>
#endif

#include <backend/process/process_store.hpp>
#include <backend/program_options.hpp>
#include <utility/resources.hpp>
#include <nui/backend/filesystem/special_paths.hpp>

#include <nui/core.hpp>
#include <nui/rpc.hpp>
#include <nui/window.hpp>
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
#include <unordered_map>
#include <cstdlib>

#ifdef __linux__
#    include <signal.h>
#    include <unistd.h>
#endif

#ifndef NDEBUG
#    include <build_environment.hpp>
#endif

using namespace std::string_literals;
using namespace std::chrono_literals;
using namespace Nui;

#ifdef __linux__
volatile sig_atomic_t sigchld[10] = {0};
#endif

namespace
{
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

    auto readFile(std::filesystem::path const& path)
    {
        std::ifstream reader{path, std::ios::binary};
        reader.seekg(0, std::ios::end);
        std::string content(reader.tellg(), '\0');
        reader.seekg(0, std::ios::beg);
        reader.read(&content[0], content.size());
        return content;
    };

    CustomScheme createFolderMapping(std::filesystem::path const& programDir, std::string const& schemeName)
    {
        return CustomScheme{
            .scheme = schemeName,
            .allowedOrigins = {"*"s},
            .onRequest =
                [programDir, schemeName, resourceDir = programDir.parent_path()](CustomSchemeRequest const& request)
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
                    Log::error("File not found: '{}'", file.string());
                    return CustomSchemeResponse{
                        .statusCode = 404,
                        .reasonPhrase = "Not Found",
                        .headers =
                            {
                                {"Content-Type"s, "text/plain"s},
                                // Do not forget to allow CORS
                                {"Access-Control-Allow-Origin"s, "*"s},
                            },
                        .body = "Not Found: "s + file.string(),
                    };
                }

                Log::debug("Serving file: '{}'", file.string());

                // Read file
                auto content = readFile(file);

                // Return file
                const auto code = content.empty() ? 204 : 200;
                return makeResponse(
                    code,
                    "OK",
                    std::move(content),
                    Roar::extensionToMime(file.extension().string()).value_or("application/octet-stream")
                );
            },

            // Windows: Is this secure like https (not http)? A lot of things are not allowed in http.
            .treatAsSecure = true,

            // Windows: Do urls to this custom scheme have an authority component? (For portability reasons, they
            // usually should have).
            .hasAuthorityComponent = true,
        };
    }
}

Main::LoggerSetup::LoggerSetup(Persistence::StateHolder& stateHolder)
{
    stateHolder.load(
        [](std::optional<std::string> const&, Persistence::StateHolder& holder, std::optional<std::string> const&)
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
    , programDir_{boost::dll::program_location().parent_path().string()}
    , stateHolder_{programDir_}
    , loggerSetup_{stateHolder_}
    , window_{
          Nui::WindowOptions{
              .title = "NuiSftp"s,
#ifdef NDEBUG
              .debug = options.enableDevTools,
#else
              .debug = true,
#endif
              .customSchemes = {createFolderMapping(programDir_, "nui")},
              .onRpcAliveMessage = [this]() {
                  onRpcAlive();
              }
          },
      }
    , hub_{window_}
    , opener_{}
    , rpcFilesystem_{}
    , rpcSystem_{}
    , processes_{window_.getExecutor(), window_, hub_, forkPool}
    , prompter_{hub_}
    , sshSessionManager_{std::make_shared<SessionManager>(window_.getExecutor(), stateHolder_, window_, hub_)}
    , childSignalTimer_{window_.getExecutor()}
    , events_{hub_}
    , themeFinder_{programDir_.parent_path(), events_}
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

    defaultPlacesProvider_ = std::make_unique<NuiFileExplorer::DefaultPlacesProvider>(hub_);
#ifdef _WIN32
    windowsDrivesProvider_ = std::make_unique<NuiFileExplorer::WindowsDrivesProvider>(hub_);
#endif

    stateHolder_.load(
        [this](
            std::optional<std::string> const& error,
            Persistence::StateHolder& holder,
            std::optional<std::string> const& warning
        )
        {
            if (warning)
            {
                Log::warn("Warning loading state: {}", *warning);
                initialPersistenceLoadWarning_ = *warning;
            }

            opener_ = std::make_unique<Opener>(window_.getNativeWindow());
            rpcSystem_ = std::make_unique<RpcSystem>(window_.getExecutor(), window_, hub_);
            tempDirInstanceManager_ = std::make_unique<FileTracking::TempDirInstanceManager>(
                window_.getExecutor(),
                window_,
                hub_,
                Nui::resolvePath(holder.stateCache().localFilesystemOptions.temporaryDownloadsDirectory.value_or(
                    Persistence::LocalFilesystemOptions{}.temporaryDownloadsDirectory.value()
                ))
            );
            tempDirInstanceManager_->registerRpc();

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
                return;
            }

            rpcFilesystem_ = std::make_unique<RpcFilesystem>(
                window_.getExecutor(), window_, hub_, holder.stateCache().localFilesystemOptions, *opener_
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

void Main::registerInitialWarningGetter()
{
    hub_.registerFunction(
        "Main::getInitialPersistenceLoadWarning",
        [this](std::string responseId)
        {
            Log::debug("Received request for initial persistence load warning.");

            const bool runningAsRoot =
#ifdef __linux__
                ::geteuid() == 0;
#else
                false;
#endif

            hub_.callRemote(
                responseId,
                nlohmann::json{
                    {"warning", initialPersistenceLoadWarning_},
                    {"isRoot", runningAsRoot},
                }
            );
        }
    );
}

void Main::startChildSignalTimer()
{
    if (shuttingDown_)
        return;

#ifdef __linux__
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
    auto options = parseProgramOptions(argc, argv);
    if (!options)
        return 0;

#ifdef __linux__

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
        .sa_restorer = nullptr,
    };

    sigaction(SIGCHLD, &sa, nullptr);

    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
#    pragma clang diagnostic pop
#endif

#ifdef _WIN32
    ForkPool* forkPoolPtr = nullptr;
#endif

    ssh_init();

    {
        Main m{std::move(*options), forkPoolPtr};
        m.startChildSignalTimer();
        m.show();
    }

    ssh_finalize();
}