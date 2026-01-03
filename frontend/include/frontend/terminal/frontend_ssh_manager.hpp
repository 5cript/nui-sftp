#pragma once

#include <frontend/terminal/terminal_engine.hpp>
#include <persistence/state/terminal_options.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/val.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class TerminalChannel
{
  public:
    TerminalChannel(MultiChannelTerminalEngine* engine, Ids::ChannelId channelId);
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
    void dispose(std::function<void()> onComplete);
    std::string stealTerminal();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};

class FrontendSshManager
{
  public:
    FrontendSshManager(std::unique_ptr<TerminalEngine> engine, bool isMultiChannel);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FrontendSshManager);

    void open(std::function<void(bool, std::string const&)> onOpen);

    void createChannel(
        Nui::val host,
        Persistence::TerminalOptions const& options,
        std::function<void(std::optional<Ids::ChannelId> /*channelId*/, std::string const& info)> onChannelCreated
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

  private:
    bool isBeingDisposed() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};