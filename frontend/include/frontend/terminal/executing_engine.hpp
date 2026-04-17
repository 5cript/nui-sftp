#pragma once

#include <persistence/state/termios.hpp>
#include <frontend/terminal/terminal_engine.hpp>
#include <frontend/terminal/channel_creation_options.hpp>
#include <roar/detail/pimpl_special_functions.hpp>
#include <persistence/state/session_options.hpp>
#include <ids/ids.hpp>
#include <nui/utility/move_detector.hpp>

#include <memory>
#include <string>

/**
 * @brief Per-call options required by ExecutingTerminalEngine::createChannel.
 *
 * The engine reads fresh command / environment / termios from this struct each
 * time a channel is spawned — no engine-wide configuration is captured at
 * construction. This keeps settings changes live (no app restart) and lets a
 * single engine spawn different shells (bash, msys2, powershell, …) as
 * sibling channels inside the same session.
 */
struct ExecutingChannelCreationOptions : ChannelCreationOptions
{
    Persistence::ExecutingSessionOptions executingOptions;
    Persistence::Termios termios;
};

/**
 * @brief Terminal engine that manages local processes as channels.
 *
 * open() succeeds immediately (no connection to establish).
 * Each createChannel() call spawns one new process via ProcessStore::spawn
 * using the ExecutingChannelCreationOptions passed in.
 * The process UUID returned by the backend is used as the ChannelId so that
 * write/resize/close operations can be routed without an extra lookup layer.
 */
class ExecutingTerminalEngine : public TerminalEngine
{
  public:
    struct Settings
    {
        std::function<void(Ids::ChannelId const&, std::string)> onProcessChange;
    };

  public:
    ExecutingTerminalEngine(Settings settings);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ExecutingTerminalEngine);

    /** @brief Signals success immediately — local processes need no prior connection. */
    void open(std::function<void(bool, std::string const&)> onOpen) override;
    void dispose(std::function<void()> onDisposeComplete) override;

    /** @brief Returns the engine-level identifier (not a process UUID). */
    std::string id() const;

    std::string engineName() const override
    {
        return "local";
    }

    /**
     * @brief Spawns a new local process.
     *
     * @p options must be an ExecutingChannelCreationOptions. If the dynamic
     * cast fails, onCreated is invoked with std::nullopt and an error message.
     */
    void createChannel(
        ChannelCreationOptions const& options,
        std::function<void(std::string const&)> handler,
        std::function<void(std::string const&)> errorHandler,
        std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated,
        std::function<void(Ids::ChannelId const&)> onChannelLoss
    ) override;

    void createSftpChannel(
        std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated
    ) override;

    void closeChannel(Ids::ChannelId const& channelId, std::function<void()> onClose = []() {}) override;
    ChannelInterface* channel(Ids::ChannelId const& channelId) override;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
