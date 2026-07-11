#include <persistence/state_holder.hpp>
#include <persistence/state/state.hpp>
#include <utility/resources.hpp>
#include <constants/persistence.hpp>
#include <log/log.hpp>
#include <yaml-cpp/yaml.h>
#include <build_environment.hpp>

#include <fmt/chrono.h>
#include <nui/backend/filesystem/special_paths.hpp>

#include <filesystem>
#include <fstream>
#include <chrono>

using namespace std::string_literals;
using namespace std::chrono_literals;

namespace Persistence
{
    namespace
    {
        void setupPersistence()
        {
            const auto path = Nui::resolvePath(Constants::persistencePath);
            const auto parentPath = std::filesystem::path{path}.parent_path().string();

            if (!std::filesystem::exists(parentPath))
                std::filesystem::create_directories(parentPath);
        }

        auto makeBackup(std::filesystem::path const& path)
        {
            const auto backupFileName = [&path]()
            {
                const auto now = std::chrono::system_clock::now();
                const auto time = fmt::format("{:%Y-%m-%d_%H-%M-%S}", now);

                return path.parent_path() / (path.filename().string() + ".backup_" + time);
            }();

            {
                std::ifstream reader{path, std::ios_base::binary};
                std::ofstream writer{backupFileName, std::ios_base::binary};

                writer << reader.rdbuf();
            }
            Log::info("Copied config file to backup: {}", backupFileName.string());
        };
    }

    void StateHolder::load(
        std::function<void(
            std::optional<std::string> const& error,
            StateHolder&,
            std::optional<std::string> const& warning
        )> const& onLoad
    )
    {
        setupPersistence();
        const auto path = Nui::resolvePath(Constants::persistencePath);

        std::optional<std::string> error{std::nullopt};
        bool fileExisted = false;
        try
        {
            const auto before = [&path, &error, &fileExisted]()
            {
                try
                {
                    std::ifstream reader{path, std::ios_base::binary};
                    if (!reader.good())
                    {
                        Log::warn("Config file does not exist, creating it with defaults.");
                        return nlohmann::json(nullptr);
                    }
                    fileExisted = true;
                    return nlohmann::json::parse(reader, nullptr, true, true);
                }
                catch (nlohmann::json::parse_error const& e)
                {
                    Log::error("Failed to parse config file: {}", e.what());
                    error = fmt::format("Failed to parse config file: {}", e.what());
                    makeBackup(path);
                    return nlohmann::json(nullptr);
                }
                catch (nlohmann::json::exception const& e)
                {
                    Log::error("Failed to parse config file: {}", e.what());
                    error = fmt::format("Failed to parse config file: {}", e.what());
                    makeBackup(path);
                    return nlohmann::json(nullptr);
                }
                catch (std::exception const& e)
                {
                    Log::error("Failed to parse config file: {}", e.what());
                    error = fmt::format("Failed to parse config file: {}", e.what());
                    makeBackup(path);
                    return nlohmann::json(nullptr);
                }
            }();

            auto appendWarning = [this](std::optional<std::string> const& warning)
            {
                if (!warning)
                    return;
                cachedWarning_ = cachedWarning_ ? *cachedWarning_ + "\n" + *warning : *warning;
            };

            if (before.is_null())
            {
                // Save something valid
                const auto warning = dataFixer(nlohmann::json::object());
                // On a true first run (no file existed) every default we just
                // populated is expected behaviour, not something to warn about.
                // Only surface warnings if the file existed but was unreadable.
                if (fileExisted)
                    appendWarning(warning);
                error = std::nullopt;
            }
            else
            {
                stateCache_ = State{};
                before.get_to(stateCache_);
                auto warning = dataFixer(before);
                auto missing = stateCache_.collectMissingMembers(stateCache_);
                if (!missing.empty())
                {
                    if (warning)
                    {
                        *warning += "\n";
                    }
                    else
                    {
                        warning =
                            "The following required fields were missing in the config and were set to defaults:\n";
                    }
                    for (auto const& member : missing)
                    {
                        *warning += fmt::format("- {}\n", member);
                    }
                }
                appendWarning(warning);
            }
        }
        catch (std::exception const& e)
        {
            Log::error("Failed to load config file: {}", e.what());
            error = fmt::format("Failed to load config file: {}", e.what());
        }

        onLoad(error, *this, cachedWarning_);
    }

    std::optional<std::string> StateHolder::dataFixer(nlohmann::json const& before)
    {
        const auto after = nlohmann::json(stateCache_);
        const auto diff = nlohmann::json::diff(before, after);
        bool mustSave = !diff.empty();
        std::optional<std::string> warning{std::nullopt};

        auto extendWarning = [&](std::string const& msg)
        {
            if (warning)
                *warning += "\n" + msg;
            else
                warning = msg;
        };

        if (mustSave)
        {
            Log::warn("Config diff: {}", diff.dump());

            // "Not understood by this version and were removed" only makes sense
            // for actual remove ops. Pure-add diffs are the normal case when this
            // version introduces fields the previous config didn't have -- not
            // user-facing noise.
            nlohmann::json droppedOps = nlohmann::json::array();
            for (auto const& op : diff)
            {
                if (op.is_object() && op.value("op", std::string{}) == "remove")
                    droppedOps.push_back(op);
            }
            if (!droppedOps.empty())
            {
                extendWarning(
                    fmt::format(
                        "Loaded json contains entries that are not understood by this version and were removed.\n"
                        "These might have been some typos or entries from a newer version.\nPlease check the config "
                        "file and re-apply any necessary settings.\nDiff:\n{}",
                        droppedOps.dump(4)
                    )
                );
            }
        }

        bool hasMissingDefaults = false;
        auto termiosDefault = stateCache_.termios.find("default");
        if (termiosDefault == stateCache_.termios.end())
        {
            Log::warn("Config file misses termios, adding defaults.");
            stateCache_.termios["default"] = Termios::saneDefaults();
            extendWarning("Added default termios settings.");
            mustSave = true;
            hasMissingDefaults = true;
        }

        auto terminalOptionsDefault = stateCache_.terminalOptions.find("default");
        if (terminalOptionsDefault == stateCache_.terminalOptions.end())
        {
            Log::warn("Config file misses terminal options, adding defaults.");
            stateCache_.terminalOptions["default"] = TerminalOptions{
#ifdef _WIN32
                .fontFamily = "consolas, monospace",
#else
                .fontFamily = "Inconsolata, Hack, JetBrains Mono, Terminus, Fixed, monospace",
#endif
                .fontSize = 12,
                .lineHeight = 1,
                .cursorBlink = false,
#ifdef __linux__
                .renderer = "dom",
#else
                .renderer = "webgl",
#endif
                .letterSpacing = 0,
                .theme = TerminalTheme{
                    .background = "#202020",
                    .white = "#efefef",
                },
            };
            extendWarning("Added default terminal options.");
            mustSave = true;
            hasMissingDefaults = true;
        }

        auto terminalOptionsNebula = stateCache_.terminalOptions.find("nebula");
        if (terminalOptionsNebula == stateCache_.terminalOptions.end())
        {
            Log::warn("Config file misses 'nebula' terminal options, adding defaults.");
            stateCache_.terminalOptions["nebula"] = TerminalOptions{
#ifdef _WIN32
                .fontFamily = "consolas, monospace",
#else
                .fontFamily = "Inconsolata, Hack, JetBrains Mono, Terminus, Fixed, monospace",
#endif
                .fontSize = 14,
                .lineHeight = 1.0,
                .cursorBlink = false,
#ifdef __linux__
                .renderer = "dom",
#else
                .renderer = "webgl",
#endif
                .letterSpacing = 0,
                .theme = TerminalTheme{
                    .background = "#0e0e14",
                    .black = "#0a0a14",
                    .blue = "#7aa2f7",
                    .brightBlack = "#3a3a4a",
                    .brightBlue = "#9bbefb",
                    .brightCyan = "#88e0e8",
                    .brightGreen = "#a7f3c5",
                    .brightMagenta = "#c4a8ff",
                    .brightRed = "#ff8585",
                    .brightWhite = "#ecebf2",
                    .brightYellow = "#f4d999",
                    .cursor = "#5fc8d4",
                    .cursorAccent = "#0e0e14",
                    .cyan = "#5fc8d4",
                    .foreground = "#ecebf2",
                    .green = "#7ee8a2",
                    .magenta = "#a78bfa",
                    .red = "#e25c5c",
                    .selectionBackground = "rgba(140, 110, 220, 0.35)",
                    .selectionForeground = "#ecebf2",
                    .selectionInactiveBackground = "rgba(140, 110, 220, 0.18)",
                    .white = "#c5c2d4",
                    .yellow = "#e5c87a",
                },
            };
            extendWarning("Added 'nebula' terminal options preset.");
            mustSave = true;
            hasMissingDefaults = true;
        }

        auto sshDefault = stateCache_.sshOptions.find("default");
        if (sshDefault == stateCache_.sshOptions.end())
        {
            Log::warn("Config file misses ssh options, adding defaults.");
            stateCache_.sshOptions["default"] = SshOptions{
#ifdef __linux__
                .tryAgentForAuthentication = true,
                .usePublicKeyAutoAuth = true,
#endif
                .usePasswordAuth = true,
                .logVerbosity = SshLogVerbosity::Off,
                .keyExchangeAlgorithms =
                    "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,"
                    "ecdh-sha2-nistp384,ecdh-sha2-nistp521,diffie-hellman-group1-sha1,diffie-hellman-group14-sha1",
                .strictHostKeyCheck = true,
#ifdef __WIN32
                .bypassConfig = true,
#endif
                .connectTimeoutSeconds = 5,
                .localeEnv = "en_US.UTF-8",
            };
            extendWarning("Added default ssh options.");
            mustSave = true;
            hasMissingDefaults = true;
        }

        auto sftpDefault = stateCache_.sftpOptions.find("default");
        if (sftpDefault == stateCache_.sftpOptions.end())
        {
            Log::warn("Config file misses sftp options, adding defaults.");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-designator"
            stateCache_.sftpOptions["default"] = SftpOptions{
                .downloadOptions =
                    DownloadOptions{
                        CommonTransferOptions{
                            .tempFileSuffix = ".filepart",
                            .mayOverwrite = false,
                            .tryContinue = true,
                            .inheritPermissions = true,
#ifdef __linux__
                            .symlinkHandling = SymlinkHandling::AsSymlink,
#elif defined(_WIN32)
                            .symlinkHandling = SymlinkHandling::FollowSymlink,
#endif
                            .failFast = false
                        },
                        .reserveSpace = false,
                        .doCleanup = true,
                    },
                .uploadOptions =
                    UploadOptions{
                        CommonTransferOptions{
                            .tempFileSuffix = ".filepart",
                            .mayOverwrite = false,
                            .tryContinue = true,
                            .inheritPermissions = true,
#ifdef __linux__
                            .symlinkHandling = SymlinkHandling::AsSymlink,
#elif defined(_WIN32)
                            .symlinkHandling = SymlinkHandling::FollowSymlink,
#endif
                            .failFast = false
                        },
                    },
                .concurrency = 1,
                .operationTimeout = 5s
            };
#pragma clang diagnostic pop
            extendWarning("Added default sftp options.");
            mustSave = true;
            hasMissingDefaults = true;
        }

        auto queueOptionsDefault = stateCache_.queueOptions.find("default");
        if (queueOptionsDefault == stateCache_.queueOptions.end())
        {
            Log::warn("Config file misses queue options, adding defaults.");
            stateCache_.queueOptions["default"] = QueueOptions{
                .startInPausedState = true,
            };
            extendWarning("Added default queue options.");
            mustSave = true;
            hasMissingDefaults = true;
        }

        auto historyOptionsDefault = stateCache_.historyOptions.find("default");
        if (historyOptionsDefault == stateCache_.historyOptions.end())
        {
            Log::warn("Config file misses history options, adding defaults.");
            stateCache_.historyOptions["default"] = HistoryOptions{
                .captureMode = HistoryCaptureMode::smart,
            };
            extendWarning("Added default history options.");
            mustSave = true;
            hasMissingDefaults = true;
        }

        // Sessions written before the command history existed carry no reference, so the global
        // history settings would not reach them. Point them at the default profile once.
        for (auto& [sessionName, session] : stateCache_.sessions)
        {
            if (session.historyOptions.hasReference())
                continue;
            Log::warn("Session '{}' misses a history options reference, pointing it at the default.", sessionName);
            session.historyOptions.ref("default");
            mustSave = true;
            hasMissingDefaults = true;
        }

        //         if (stateCache_.sessions.empty())
        //         {
        //             Log::warn("Config file misses terminal engines, adding defaults.");
        // #ifdef _WIN32
        //             stateCache_.sessions["msys2_default"] = SessionOptions{
        //                 .type = TerminalEngineType::shell,
        //                 .engine = defaultMsys2SessionOption(),
        //                 .terminalOptions = Reference{"default"},
        //                 .termios = Reference{"default"},
        //             };
        //             extendWarning("Added default msys2 terminal engine.");
        // #elif __APPLE__
        // // nothing
        // #else
        //             stateCache_.sessions["bash_default"] = SessionOptions{
        //                 .type = TerminalEngineType::shell,
        //                 .engine = defaultBashSessionOption(),
        //                 .terminalOptions = Reference{"default"},
        //                 .termios = Reference{"default"},
        //                 .queueOptions = Reference{"default"},

        //             };
        //             extendWarning("Added default bash terminal engine.");
        // #endif
        //             mustSave = true;
        //             hasMissingDefaults = true;
        //         }

        if (hasMissingDefaults)
        {
            extendWarning("Wrote missing default settings to config file.");
        }
        if (mustSave)
        {
            Log::warn("Config file misses some defaults or has misunderstood parameters, writing them back to disk.");
            save();
        }
        return warning;
    }

    void StateHolder::clearWarnings(std::function<void()> const& onComplete)
    {
        cachedWarning_ = std::nullopt;
        onComplete();
    }

    void StateHolder::save(std::function<void(std::optional<std::string> const& error)> const& onSaveComplete)
    {
        setupPersistence();
        const auto path = Nui::resolvePath(Constants::persistencePath);

        try
        {
            std::ofstream writer{path, std::ios_base::binary};
            writer << nlohmann::json(stateCache_).dump(4);
            onSaveComplete(std::nullopt);
        }
        catch (std::exception const& e)
        {
            Log::error("Failed to save config file: {}", e.what());
            throw;
        }
    }

    void StateHolder::registerRpc(Nui::RpcHub& hub)
    {
        hub.registerFunction(
            "StateHolder::load",
            [&hub, this](std::string responseId)
            {
                Log::debug("Received state load request from frontend state holder.");

                load(
                    [responseId, &hub](
                        std::optional<std::string> const& error,
                        StateHolder& holder,
                        std::optional<std::string> const& warning
                    )
                    {
                        auto json = nlohmann::json::object();
                        if (warning)
                            json["warning"] = *warning;
                        if (error)
                        {
                            json["error"] = *error;
                            hub.callRemote(responseId, json);
                            return;
                        }
                        json["state"] = nlohmann::json(holder.stateCache()).dump();
                        Log::debug("State loaded from disk.");
                        hub.callRemote(responseId, json);
                    }
                );
            }
        );

        hub.registerFunction(
            "StateHolder::save",
            [&hub, this](std::string responseId, std::string const& state)
            {
                Log::debug("Received state save request from frontend state holder.");

                try
                {
                    stateCache_ = nlohmann::json::parse(state).get<State>();
                    save(
                        [&hub, responseId](std::optional<std::string> const& error)
                        {
                            if (error)
                            {
                                hub.callRemote(
                                    responseId,
                                    nlohmann::json{
                                        {"error", *error},
                                    }
                                );
                                return;
                            }
                            Log::debug("State saved to disk.");
                            hub.callRemote(responseId, nlohmann::json{{"success", true}});
                        }
                    );
                }
                catch (std::exception const& e)
                {
                    hub.callRemote(
                        responseId,
                        nlohmann::json{
                            {"error", fmt::format("Failed to save state to disk: {}", e.what())},
                        }
                    );
                }
            }
        );

        hub.registerFunction(
            "StateHolder::clearWarnings",
            [&hub, this](std::string responseId)
            {
                cachedWarning_ = std::nullopt;
                hub.callRemote(responseId, nlohmann::json{{"success", true}});
            }
        );

        hub.registerFunction(
            "StateHolder::loadLanguageFiles",
            [this, &hub](std::string responseId)
            {
                Log::debug("Received language file load request from frontend state holder.");

                loadLanguageFiles(
                    [responseId, &hub](std::optional<nlohmann::json> const& jsonOpt)
                    {
                        if (jsonOpt)
                        {
                            auto response = nlohmann::json::object();
                            response["jsonString"] = jsonOpt->dump();
                            hub.callRemote(responseId, response);
                        }
                        else
                        {
                            hub.callRemote(
                                responseId,
                                nlohmann::json{
                                    {"error", "Failed to load language file."},
                                }
                            );
                        }
                    }
                );
            }
        );
    }

    void StateHolder::loadLanguageFile(
        std::filesystem::path const& filePath,
        std::function<void(std::optional<nlohmann::json> const&)> const& onLoadComplete
    )
    {
        // Convert to json:
        nlohmann::json json;

        try
        {
            YAML::Node config = YAML::LoadFile(filePath.generic_string());
            auto translateNode = [&](this const auto& translateNode, YAML::Node const& node) -> nlohmann::json
            {
                if (node.IsScalar())
                {
                    return node.as<std::string>();
                }
                else if (node.IsMap())
                {
                    nlohmann::json obj = nlohmann::json::object();
                    for (auto const& item : node)
                    {
                        obj[item.first.as<std::string>()] = translateNode(item.second);
                    }
                    return obj;
                }
                return nullptr;
            };
            json = translateNode(config);
        }
        catch (std::exception const& e)
        {
            Log::error("Failed to parse language file: {}", e.what());
            onLoadComplete(std::nullopt);
            return;
        }
        onLoadComplete(std::move(json));
    }

    void StateHolder::loadLanguageFiles(std::function<void(std::optional<nlohmann::json> const&)> const& onLoadComplete)
    {
        const auto files = findFilesInSearchPaths(programDirectory_.parent_path(), "assets/languages/*.yaml");
        if (files.empty())
        {
            Log::error("No language files found in any search path.");
            onLoadComplete(std::nullopt);
            return;
        }

        nlohmann::json assembled = nlohmann::json::object();
        for (const auto& file : files)
        {
            std::optional<nlohmann::json> content;
            loadLanguageFile(
                file,
                [&content](const auto maybeJson)
                {
                    content = maybeJson;
                }
            );
            if (!content)
            {
                Log::error("Failed to load language file: {}", file.string());
                onLoadComplete(std::nullopt);
                return;
            }
            assembled[file.stem().string()] = *content;
        }
        onLoadComplete(std::move(assembled));
    }
}