#pragma once

#include <frontend/terminal/terminal_engine.hpp>
#include <persistence/state/terminal_options.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/val.hpp>

#include <functional>
#include <memory>
#include <string>

class TerminalChannel
{
  public:
    TerminalChannel(
        TerminalEngine* engine,
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
    /**
     * @brief Writes a serialized scrollback dump (output of getAllTextContent)
     *        into the xterm instance.  Used to restore history after a
     *        seamless reconnect swap.  Call after the terminal has been opened
     *        but before any fresh live output arrives.
     * @param serializedDump The xterm serializeAddon output to replay.
     */
    void replayContent(std::string const& serializedDump);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
