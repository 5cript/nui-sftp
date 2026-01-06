#include <persistence/state_holder.hpp>
#include <persistence/state/state.hpp>
#include <constants/persistence.hpp>
#include <log/log.hpp>

#include <fmt/chrono.h>
#include <roar/filesystem/special_paths.hpp>

#include <filesystem>
#include <fstream>
#include <chrono>

namespace Persistence
{
    namespace
    {
        void setupPersistence()
        {
            const auto path = Roar::resolvePath(Constants::persistencePath);
            const auto parentPath = std::filesystem::path{path}.parent_path().string();

            if (!std::filesystem::exists(parentPath))
                std::filesystem::create_directories(parentPath);
        }
    }

    void StateHolder::load(std::function<void(std::optional<std::string> const& /*error*/, StateHolder&)> const& onLoad)
    {
        setupPersistence();
        const auto path = Roar::resolvePath(Constants::persistencePath);

        auto makeBackup = [&path]()
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

        try
        {
            std::optional<std::string> error{std::nullopt};
            const auto before = [&path, &makeBackup, &error]()
            {
                try
                {
                    std::ifstream reader{path, std::ios_base::binary};
                    if (!reader.good())
                    {
                        Log::warn("Config file does not exist, creating it with defaults.");
                        return nlohmann::json(nullptr);
                    }
                    return nlohmann::json::parse(reader, nullptr, true, true);
                }
                catch (nlohmann::json::parse_error const& e)
                {
                    Log::error("Failed to parse config file: {}", e.what());
                    error = fmt::format("Failed to parse config file: {}", e.what());
                    makeBackup();
                    return nlohmann::json(nullptr);
                }
                catch (nlohmann::json::exception const& e)
                {
                    Log::error("Failed to parse config file: {}", e.what());
                    error = fmt::format("Failed to parse config file: {}", e.what());
                    makeBackup();
                    return nlohmann::json(nullptr);
                }
                catch (std::exception const& e)
                {
                    Log::error("Failed to parse config file: {}", e.what());
                    error = fmt::format("Failed to parse config file: {}", e.what());
                    makeBackup();
                    return nlohmann::json(nullptr);
                }
            }();

            if (before.is_null())
            {
                // Save something valid
                dataFixer(nlohmann::json::object());
                onLoad(error, *this);
                return;
            }

            before.get_to(stateCache_);
            dataFixer(before);
            onLoad(error, *this);
        }
        catch (std::exception const& e)
        {
            Log::error("Failed to load config file: {}", e.what());
            onLoad(fmt::format("Failed to load config file: {}", e.what()), *this);
        }
    }

    void StateHolder::dataFixer(nlohmann::json const& before)
    {
        const auto after = nlohmann::json(stateCache_);
        bool mustSave = !nlohmann::json::diff(before, after).empty();

        if (mustSave)
        {
            Log::warn("Config diff: {}", nlohmann::json::diff(before, after).dump());
        }

        if (stateCache_.termios.empty())
        {
            Log::warn("Config file misses termios, adding defaults.");
            stateCache_.termios["default"] = Termios::saneDefaults();
            mustSave = true;
        }

        if (stateCache_.terminalOptions.empty())
        {
            Log::warn("Config file misses terminal options, adding defaults.");
            stateCache_.terminalOptions["default"] = TerminalOptions{
                .fontFamily = "consolas, courier-new, courier, monospace",
                .fontSize = 14,
                .lineHeight = std::nullopt,
                .renderer = "canvas",
                .letterSpacing = 0,
                .theme = TerminalTheme{
                    .background = "#202020",
                    .white = "#efefef",
                },
            };
            mustSave = true;
        }

        if (stateCache_.sessions.empty())
        {
            Log::warn("Config file misses terminal engines, adding defaults.");
#ifdef _WIN32
            stateCache_.sessions["msys2_default"] = TerminalEngine{
                .type = "shell",
                .terminalOptions = Reference{.ref = "default"},
                .termios = Reference{.ref = "default"},
                .engine = defaultMsys2TerminalEngine(),
            };
#elif __APPLE__
// nothing
#else
            stateCache_.sessions["bash_default"] = TerminalEngine{
                .type = "shell",
                .terminalOptions = Reference{.ref = "default"},
                .termios = Reference{.ref = "default"},
                .engine = defaultBashTerminalEngine(),
            };
#endif
            mustSave = true;
        }

        if (mustSave)
        {
            Log::warn("Config file misses some defaults, writing them back to disk.");
            save();
        }
    }

    void StateHolder::save(std::function<void(std::optional<std::string> const& error)> const& onSaveComplete)
    {
        setupPersistence();
        const auto path = Roar::resolvePath(Constants::persistencePath);

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
                    [responseId, &hub](std::optional<std::string> const& error, StateHolder& holder)
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
                        Log::debug("State loaded from disk.");
                        hub.callRemote(responseId, nlohmann::json(holder.stateCache_).dump());
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
    }
}