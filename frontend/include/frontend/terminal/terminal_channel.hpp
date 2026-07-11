#pragma once

#include <frontend/terminal/terminal_engine.hpp>
#include <persistence/state/history_options.hpp>
#include <persistence/state/terminal_options.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/val.hpp>

#include <functional>
#include <memory>
#include <string>

class TerminalChannel
{
  public:
    /**
     * @param captureMode How commands run in this terminal are picked up for the command history.
     *                    Selected once here, the strategy never changes for the life of the channel.
     */
    TerminalChannel(
        TerminalEngine* engine,
        Ids::ChannelId channelId,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput,
        Persistence::HistoryCaptureMode captureMode = Persistence::HistoryCaptureMode::off
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

    /**
     * @brief Sets the sink for commands executed in this terminal.
     *
     * Fed by the OSC 633 handler in smart mode and by the keystroke line buffer in simple mode; in
     * off mode nothing ever calls it. Set it before open(), the handlers are registered there.
     */
    void setOnCommandExecuted(std::function<void(std::string const&)> onCommandExecuted);

    /**
     * @brief Writes a shell integration bootstrap line into the shell's stdin.
     *
     * Only does something in smart mode and only on a freshly opened channel; an adopted or replayed
     * channel already has its hook. An empty line is a no-op, which is what an unknown local shell
     * gets.
     *
     * @param bootstrapLine One of the lines from the ShellIntegration namespace, without a newline.
     */
    void installShellIntegration(std::string const& bootstrapLine);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
