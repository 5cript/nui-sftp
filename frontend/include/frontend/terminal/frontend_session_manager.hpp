#pragma once

#include <frontend/terminal/terminal_channel.hpp>
#include <frontend/terminal/terminal_engine.hpp>
#include <frontend/terminal/executing_engine.hpp>
#include <persistence/state/terminal_options.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/val.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

/**
 * @brief Manages the terminal-side lifecycle of a single transport session.
 *
 * Owns a primary TerminalEngine (the backend transport: SSH or local process)
 * plus an optional auxiliary ExecutingTerminalEngine that hosts local-shell
 * panels inside an SSH session. The aux engine is lazy — created only on
 * first use — and its channels are isolated from primary-engine lifecycle
 * events (connection loss, broadcast, save-on-disconnect).
 *
 * @note The "Frontend" prefix reflects that this is the frontend counterpart of
 *       the backend session object.  A more concise name would be
 *       TerminalSession.
 */
class FrontendSessionManager
{
  public:
    /**
     * @brief Filter selector for forEachChannel / broadcast / connectionLossMode.
     */
    enum class EngineFilter
    {
        AllChannels,
        PrimaryOnly,
        LocalShellOnly
    };

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
     * @brief Creates a new primary-engine channel and binds an xterm.js terminal to it.
     *
     * @p channelOptions is the polymorphic per-call options struct — pass an
     * ExecutingChannelCreationOptions for shell-only sessions, or a plain
     * ChannelCreationOptions for SSH (which ignores it).
     *
     * @param host              DOM element that will host the xterm.js terminal.
     * @param options           Terminal appearance and behaviour options.
     * @param channelOptions    Engine-specific per-call options.
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
        ChannelCreationOptions const& channelOptions,
        std::function<void(std::optional<Ids::ChannelId> /*channelId*/, std::string const& info)> onChannelCreated,
        std::function<void(Ids::ChannelId const&)> onChannelLoss
    );

    /**
     * @brief Creates a local-shell channel on the auxiliary engine.
     *
     * Lazily constructs an ExecutingTerminalEngine on first call; subsequent
     * calls reuse it. Isolated from primary-engine lifecycle events:
     * connection-loss mode and broadcasts do not touch these channels, and
     * primary-engine disposal is independent.
     *
     * @param host               DOM element for the xterm.js terminal.
     * @param terminalOptions    Terminal appearance and behaviour options.
     * @param shellOptions       The saved shell's ExecutingSessionOptions.
     * @param termios            The saved shell's termios.
     * @param onProcessChange    Fired when the shell's foreground process
     *                           changes (used to update tab title / cmdline).
     * @param onChannelCreated   Called with (channelId, "") on success, or
     *                           (nullopt, reason) on failure.
     * @param onChannelLoss      Called when the process exits.
     */
    void createLocalShellChannel(
        Nui::val host,
        Persistence::TerminalOptions const& terminalOptions,
        Persistence::ExecutingSessionOptions const& shellOptions,
        Persistence::Termios const& termios,
        std::function<void(Ids::ChannelId const&, std::string)> onProcessChange,
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
     * @brief Returns the engine that owns @p channelId, or nullptr.
     *
     * Used by callers that need to route per-channel operations (e.g. close,
     * resize) explicitly, or to distinguish primary from local-shell channels.
     */
    TerminalEngine* engineOf(Ids::ChannelId const& channelId);

    /**
     * @brief True if @p channelId is owned by the auxiliary local-shell engine.
     */
    bool isLocalShellChannel(Ids::ChannelId const& channelId);

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
     * @brief Calls @p handler for every matching channel, stopping early on false.
     *
     * No-op when the manager is being disposed.
     *
     * @param filter   Which engine's channels to iterate.
     * @param handler  Called with (channelId, channel).  Return true to
     *                 continue, false to stop iteration.
     */
    void forEachChannel(
        EngineFilter filter,
        std::function<bool(Ids::ChannelId const&, TerminalChannel&)> const& handler
    );

    /** @brief Iterates all channels (convenience wrapper for EngineFilter::AllChannels). */
    void forEachChannel(std::function<bool(Ids::ChannelId const&, TerminalChannel&)> const& handler);

    /**
     * @brief Tears down all channels and both engines, then calls @p onComplete.
     *
     * Idempotent: once the first dispose has completed, subsequent calls
     * (including from the destructor) invoke @p onComplete and return
     * immediately.  Re-entrant calls while a dispose is already in progress
     * also return immediately.
     *
     * Backend channels are intentionally not closed individually — each
     * engine's own dispose handles transport teardown.
     *
     * @param onComplete  Called when teardown is finished.
     */
    void dispose(std::function<void()> onComplete);

    /**
     * @brief Returns a reference to the primary TerminalEngine.
     */
    TerminalEngine& engine();

    /**
     * @brief Focuses the xterm.js widget of the first channel, if any exists.
     */
    void focusFirst();

    /**
     * @brief Writes @p msg to matching channels as non-user-input data.
     *
     * Intended for system messages (e.g. "connection lost") — the filter
     * defaults to PrimaryOnly so local-shell channels stay clean when the
     * SSH transport drops.  No-op when the manager is being disposed.
     *
     * @param msg     Text to write; newlines are normalised to CR+LF by
     *                TerminalChannel.
     * @param filter  Which engine's channels to write to.
     */
    void broadcast(std::string const& msg, EngineFilter filter = EngineFilter::PrimaryOnly);

    /**
     * @brief Enables or disables connection-loss mode.
     *
     * By default targets only primary-engine channels — local-shell channels
     * are independent of the SSH transport and must keep functioning when
     * SSH drops. When @p active is true, user input is routed to the
     * onLockedUserInput callback instead of the backend and resize events
     * are suppressed. Pass false to restore normal operation after
     * reconnection.
     *
     * @param active  True to enter connection-loss mode, false to leave it.
     * @param filter  Which engine's channels to lock (default: PrimaryOnly).
     */
    void connectionLossMode(bool active, EngineFilter filter = EngineFilter::PrimaryOnly);

  private:
    /// Logs a warning and returns true when the manager is being disposed.
    /// Used as a disposal guard by public methods.
    bool guardDisposal() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
