#pragma once

#include <persistence/state/termios.hpp>
#include <frontend/terminal/terminal_engine.hpp>
#include <roar/detail/pimpl_special_functions.hpp>
#include <persistence/state/session_options.hpp>
#include <nui/utility/move_detector.hpp>

#include <memory>
#include <string>

class ExecutingTerminalEngine : public TerminalEngine
{
  public:
    struct Settings
    {
        Persistence::ExecutingSessionOptions engineOptions;
        Persistence::Termios termios;
        std::function<void(std::string)> onProcessChange;
    };

  public:
    ExecutingTerminalEngine(Settings settings);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ExecutingTerminalEngine);

    void open(std::function<void(bool, std::string const&)> onOpen) override;
    void dispose(std::function<void()> onDisposeComplete) override;
    std::string id() const;
    std::string engineName() const override
    {
        return "local";
    }

    // TODO: Map these onto createChannel/closeChannel once multi-channel support is implemented for local processes.
    void write(std::string const& data);
    void resize(int cols, int rows);
    void setStdoutHandler(std::function<void(std::string const&)> handler);
    void setStderrHandler(std::function<void(std::string const&)> handler);

    // TerminalEngine channel interface — not yet implemented for local processes.
    void createChannel(
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
    void updatePtyProcs();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
