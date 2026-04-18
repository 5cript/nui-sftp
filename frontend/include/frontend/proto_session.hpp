#pragma once

#include <frontend/session_snapshot.hpp>
#include <frontend/terminal/frontend_session_manager.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state/ui_options.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

/**
 * @brief Headless stand-in for a Session that only opens the transport.
 *
 * A ProtoSession constructs a FrontendSessionManager and calls open() on it,
 * nothing more.  No DOM is attached, no file grid is built, no panels are
 * wired, no reconnect UI lives on it.  Its single responsibility is to answer
 * the question "can we reach the backend session engine?" and, on success,
 * hand its opened FSM to a full Session via the adoption constructor.
 *
 * Used by SessionArea during reconnect cycles: on Reconnect click the old
 * Session stays rendered (with its lost-connection overlay on top), while
 * a ProtoSession probes the transport in the background.  When it fires
 * onReady, SessionArea tears down the old Session and constructs a new one
 * that consumes the ProtoSession — the ProtoSession is destroyed at the end
 * of that constructor, never visible to the user.
 *
 * ProtoSession does not hold the retry state.  SessionArea owns the retry
 * timers + attempt counter and spawns a fresh ProtoSession on each attempt.
 */
class ProtoSession
{
  public:
    struct Params
    {
        /// Engine options (SSH connection details, termios, terminal options).
        Persistence::SessionOptions sessionOptions{};
        /// UiOptions carried through to the adopting Session untouched.
        Persistence::UiOptions uiOptions{};
        /// Snapshot captured from the dying Session — carried through to the
        /// adopting Session so scrollback, layout, file grid state, etc. can
        /// all be restored on the new side.
        std::optional<SessionSnapshot> resumeFromSnapshot{};
        /// Fired when FSM::open succeeds.  The ProtoSession stays alive; the
        /// caller (SessionArea) is expected to route it into a Session
        /// adoption constructor and destroy it there.
        std::function<void(ProtoSession*)> onReady{};
        /// Fired when FSM::open fails.  The ProtoSession is still alive and
        /// can be destroyed freely; its FSM has already begun disposal per
        /// FrontendSessionManager::open's contract.
        std::function<void(ProtoSession*, std::string const&)> onFailed{};
    };

    explicit ProtoSession(Params params);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ProtoSession);

    /** @brief Kicks off the FSM open.  Safe to call exactly once. */
    void start();

    /* ── Adoption accessors ─────────────────────────────────────────────── */
    /* These transfer state out of the ProtoSession into the adopting Session.
       After the first call to each, the ProtoSession no longer owns that
       piece of state — so the Session adoption constructor should be the
       only caller, and the ProtoSession should be destroyed immediately
       afterwards. */

    Persistence::SessionOptions takeSessionOptions();
    Persistence::UiOptions takeUiOptions();
    std::unique_ptr<FrontendSessionManager> takeFrontendSessionManager();
    std::optional<SessionSnapshot> takeResumeSnapshot();

    /** @brief Engine name of the probed transport ("ssh" or "local"). */
    std::string engineName() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
