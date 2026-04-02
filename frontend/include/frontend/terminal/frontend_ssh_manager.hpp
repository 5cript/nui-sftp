#pragma once

#include <frontend/terminal/terminal_engine.hpp>
#include <persistence/state/terminal_options.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/val.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class TerminalChannel
{
  public:
    TerminalChannel(
        MultiChannelTerminalEngine* engine,
        Ids::ChannelId channelId,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    );
    virtual ~TerminalChannel();
    TerminalChannel(TerminalChannel const&) = delete;
    TerminalChannel(TerminalChannel&&);
    TerminalChannel& operator=(TerminalChannel const&) = delete;
    TerminalChannel& operator=(TerminalChannel&&);

    void open(
        Nui::val host,
        Persistence::TerminalOptions const& options,
        std::function<void(bool, std::string const&)> onOpen
    );
    bool isOpen() const;
    void write(std::string const& data, bool isUserInput);
    void writeStderr(std::string const& data, bool isUserInput);
    void focus();
    void dispose(std::function<void()> onComplete, bool closeBackendChannel = true);
    std::string stealTerminal();
    void connectionLossMode(bool isLocked);
    std::string getAllTextContent() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};

class FrontendSessionManager
{
  public:
    FrontendSessionManager(
        std::unique_ptr<TerminalEngine> engine,
        bool isMultiChannel,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FrontendSessionManager);

    void open(std::function<void(bool, std::string const&)> onOpen);

    void createChannel(
        Nui::val host,
        Persistence::TerminalOptions const& options,
        std::function<void(std::optional<Ids::ChannelId> /*channelId*/, std::string const& info)> onChannelCreated,
        std::function<void(Ids::ChannelId const&)> onChannelLoss
    );
    TerminalChannel* channel(Ids::ChannelId const& channelId);
    void closeChannel(Ids::ChannelId const& channelId);
    void closeAllChannels();

    void
    iterateAllChannels(std::function<bool(Ids::ChannelId const& channelId, TerminalChannel& channel)> const& handler);

    void dispose(std::function<void()> onComplete, bool recursion = false);
    TerminalEngine& engine();

    // Focusses the first terminal channel if it exists
    void focus();

    // This is never user input
    void writeBroadcast(std::string const& msg);

    // No more user interaction.
    void connectionLossMode(bool isLocked);

  private:
    bool isBeingDisposed() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};