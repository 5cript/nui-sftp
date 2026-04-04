#pragma once

#include <frontend/terminal/terminal_channel.hpp>
#include <frontend/terminal/terminal_engine.hpp>
#include <persistence/state/terminal_options.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/val.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

/**
 * @brief Manages the terminal-side lifecycle of a single transport session.
 *
 * Owns a TerminalEngine (the backend transport: SSH, local process, …) and
 * the set of TerminalChannels (xterm.js instances) opened on top of it.
 * Coordinates lifecycle events such as opening the engine, creating/closing
 * channels, broadcasting status messages to all channels, and tearing
 * everything down when the session ends.
 *
 * @note The "Frontend" prefix reflects that this is the frontend counterpart of
 *       the backend session object.  A more concise name would be
 *       TerminalSession.
 */
class FrontendSessionManager
{
  public:
    /**
     * @brief Constructs the manager.
     *
     * @param engine             Ownership of the backend transport engine.
     * @param onLockedUserInput  Callback invoked with (channelId, data) when a
     *                           channel is in connection-loss mode and the user
     *                           types into the xterm widget.  Allows the owner
     *                           to buffer or react to the input.
     */
    FrontendSessionManager(
        std::unique_ptr<TerminalEngine> engine,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FrontendSessionManager);

    /**
     * @brief Opens the underlying TerminalEngine.
     *
     * Must be called before createChannel().  On failure the manager begins
     * disposing itself and calls @p onOpen(false, reason).
     *
     * @param onOpen  Completion callback — (success, info).
     */
    void open(std::function<void(bool, std::string const&)> onOpen);

    /**
     * @brief Creates a new backend channel and binds an xterm.js terminal to it.
     *
     * The channel is registered in the internal map before @p onChannelCreated
     * fires, so any data written by the backend before the xterm widget is ready
     * is buffered and flushed once the terminal opens.
     *
     * @param host              DOM element that will host the xterm.js terminal.
     * @param options           Terminal appearance and behaviour options.
     * @param onChannelCreated  Called with (channelId, "") on success, or
     *                          (nullopt, reason) on failure.
     * @param onChannelLoss     Called when the backend side of the channel drops.
     *                          In connection-loss mode the channel is kept alive
     *                          so the user can read or copy its content;
     *                          otherwise it is removed immediately.
     */
    void createChannel(
        Nui::val host,
        Persistence::TerminalOptions const& options,
        std::function<void(std::optional<Ids::ChannelId> /*channelId*/, std::string const& info)> onChannelCreated,
        std::function<void(Ids::ChannelId const&)> onChannelLoss
    );

    /**
     * @brief Returns a pointer to the channel with @p channelId, or nullptr.
     *
     * Returns nullptr when the manager is being disposed or the channel is not
     * found.
     */
    TerminalChannel* channel(Ids::ChannelId const& channelId);

    /**
     * @brief Removes the channel with @p channelId from the internal map.
     *
     * The TerminalChannel destructor handles xterm.js cleanup.  No-op if the
     * channel is not found or the manager is being disposed.
     */
    void closeChannel(Ids::ChannelId const& channelId);

    /**
     * @brief Removes all channels.
     *
     * No-op when the manager is being disposed.
     */
    void closeAllChannels();

    /**
     * @brief Calls @p handler for every channel, stopping early on false.
     *
     * No-op when the manager is being disposed.
     *
     * @param handler  Called with (channelId, channel).  Return true to
     *                 continue, false to stop iteration.
     */
    void forEachChannel(
        std::function<bool(Ids::ChannelId const&, TerminalChannel&)> const& handler
    );

    /**
     * @brief Tears down all channels and the engine, then calls @p onComplete.
     *
     * Idempotent: once the first dispose has completed, subsequent calls
     * (including from the destructor) invoke @p onComplete and return
     * immediately.  Re-entrant calls while a dispose is already in progress
     * also return immediately.
     *
     * Backend channels are intentionally not closed individually — the engine's
     * own dispose handles transport teardown.
     *
     * @param onComplete  Called when teardown is finished.
     */
    void dispose(std::function<void()> onComplete);

    /**
     * @brief Returns a reference to the underlying TerminalEngine.
     */
    TerminalEngine& engine();

    /**
     * @brief Focuses the xterm.js widget of the first channel, if any exists.
     */
    void focusFirst();

    /**
     * @brief Writes @p msg to every channel as non-user-input data.
     *
     * Intended for system messages visible in all terminals (e.g. "connection
     * lost").  No-op when the manager is being disposed.
     *
     * @param msg  Text to write; newlines are normalised to CR+LF by
     *             TerminalChannel.
     */
    void broadcast(std::string const& msg);

    /**
     * @brief Enables or disables connection-loss mode on all channels.
     *
     * When @p active is true, user input is routed to the onLockedUserInput
     * callback instead of the backend and resize events are suppressed.  Pass
     * false to restore normal operation after reconnection.
     *
     * @param active  True to enter connection-loss mode, false to leave it.
     */
    void connectionLossMode(bool active);

  private:
    /// Logs a warning and returns true when the manager is being disposed.
    /// Used as a disposal guard by public methods.
    bool guardDisposal() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
