#pragma once

#include <frontend/terminal/terminal_engine.hpp>
#include <frontend/terminal/ssh_channel.hpp>
#include <roar/detail/pimpl_special_functions.hpp>
#include <persistence/state/session_options.hpp>
#include <frontend/terminal/ssh_channel.hpp>
#include <nui/utility/move_detector.hpp>
#include <ids/id.hpp>

#include <memory>
#include <string>

class SshTerminalEngine : public TerminalEngine
{
  public:
    struct Settings
    {
        Persistence::SessionOptions sessionOptions;
        std::function<void()> onConnectionLoss;
    };

  public:
    SshTerminalEngine(Settings settings);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SshTerminalEngine);

    void open(std::function<void(bool, std::string const&)> onOpen) override;

    /**
     * @brief Spawns an SSH shell channel. @p options is unused — SSH reads
     *        all relevant state from the session-level settings captured at
     *        engine construction.
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

    void closeChannel(Ids::ChannelId const& channelId, std::function<void()> onChannelClosed = []() {}) override;
    SshChannel* channel(Ids::ChannelId const& channelId) override;
    std::string stealChannelTerminal(Ids::ChannelId const& channelId);

    void dispose(std::function<void()> onDisposeComplete) override;
    std::string engineName() const override
    {
        return "ssh";
    }

    Ids::SessionId sshSessionId() const;

  private:
    void disconnect(std::function<void()> onDisconnect, bool byLossOfConnection = false);

    void onSuccessfulOpen();

    void createChannelImpl(
        std::function<void(std::string const&)> handler,
        std::function<void(std::string const&)> errorHandler,
        std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated,
        std::function<void(Ids::ChannelId const&)> onChannelLoss,
        bool fileMode
    );

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};