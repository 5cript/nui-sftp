#pragma once

#include <frontend/session_snapshot.hpp>
#include <ids/ids.hpp>

#include <nui/event_system/observed_value.hpp>

#include <nlohmann/json.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class FrontendSessionManager;
class TerminalPanel;
class FileExplorerPanel;
class OperationQueue;
class SessionLayoutInitializer;
class TerminalChannel;

/**
 * @brief Owns the Session's reconnect-related state: the pending resume
 *        snapshot, the scrollback replay queue, the local-shell adoption
 *        list, tracked bulk-resume OperationIds, cached remote username,
 *        and sftp-open flag.  Implements captureSnapshot / applySnapshot
 *        / ejectLocalShellsForHandoff / takePendingSnapshot, and discards
 *        tracked bulk resumes in its destructor (last-chance cleanup for
 *        sessions closed before applySnapshot runs or before adoption
 *        consumes them server-side).  Extracted from Session as part of
 *        the session.cpp refactor (Step 7).
 */
class SessionSnapshotManager
{
  public:
    /**
     * @brief Wiring for SessionSnapshotManager.  Takes non-owning pointers
     *        to the widgets whose state feeds into / gets restored from
     *        the snapshot.  `initialPending` carries over the reconnect
     *        snapshot the Session was constructed with, if any.
     */
    struct Params
    {
        /** @brief Non-owning reference to the Session's FSM observable. */
        Nui::Observed<std::unique_ptr<FrontendSessionManager>>* frontendSessionManager = nullptr;

        /** @brief Widgets whose state participates in the snapshot. */
        TerminalPanel* terminalPanel = nullptr;
        FileExplorerPanel* fileExplorerPanel = nullptr;
        OperationQueue* operationQueue = nullptr;
        SessionLayoutInitializer* layoutInitializer = nullptr;

        /**
         * @brief The reconnect snapshot this Session was constructed with
         *        (if any).  Consumed by hasPending / pending / reset, and
         *        seeds pendingLocalShellAdoptions on construction.
         */
        std::optional<SessionSnapshot> initialPending;
    };

    explicit SessionSnapshotManager(Params params);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SessionSnapshotManager);

    /**
     * @brief Captures the currently-visible state into a SessionSnapshot.
     *        @p withEjection controls whether local-shell channels are
     *        also evicted from the aux engine (destructive: kills the
     *        frontend wrappers, leaves backend processes alive for
     *        adoption).  SessionArea captures non-destructively at
     *        reconnect click and only ejects at the moment of swap via
     *        ejectLocalShellsForHandoff.
     */
    SessionSnapshot capture(bool withEjection);

    /** @brief Applies a snapshot produced by capture() to this session. */
    void apply(SessionSnapshot const& snapshot);

    /**
     * @brief Ejects local-shell channels from the aux engine without the
     *        rest of the capture work.  Used by SessionArea during the
     *        ProtoSession handoff the non-destructive snapshot is
     *        captured at Reconnect click, and this runs at the moment of
     *        swap so the processes survive exactly the transition.
     */
    std::vector<LocalShellAdoption> ejectLocalShellsForHandoff();

    /**
     * @brief Extracts the pending reconnect snapshot.  Called by
     *        SessionArea when a reconnect attempt's Session fails to open
     *        and the snapshot must be reused for the next retry.
     */
    std::optional<SessionSnapshot> takePendingSnapshot();

    /** @brief True while a resume snapshot is still held (not yet consumed). */
    bool hasPending() const;

    /** @brief Read the pending snapshot.  Only valid while hasPending(). */
    SessionSnapshot const& pending() const;

    /** @brief Drops the pending snapshot.  Called once the reconnect path has applied it. */
    void resetPending();

    /**
     * @brief Layout-initializer takeResumeLayout callback target: returns
     *        pendingResumeLayout (and consumes it) if present, else the
     *        pending snapshot's luminoLayout, else nullopt.
     */
    std::optional<nlohmann::json> takeResumeLayout();

    /**
     * @brief Session constructors copy the snapshot's Lumino layout here
     *        up front so layout restore can find it after the snapshot is
     *        reset by onOpenSession.
     */
    void seedPendingResumeLayout(nlohmann::json layout);

    /**
     * @brief Non-owning reference to the local-shell adoption queue.  The
     *        layout initializer's localShellFactory consumes it in place.
     */
    std::vector<LocalShellAdoption>* pendingLocalShellAdoptionsPtr();

    /** @brief Drains the front scrollback entry into the given channel if any. */
    void replayScrollbackFor(Ids::ChannelId const& channelId, TerminalChannel& channel);

    std::string const& remoteUsername() const;
    void setRemoteUsername(std::string value);

    bool sftpIsOpen() const;
    void setSftpIsOpen(bool value);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
