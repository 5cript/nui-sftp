#pragma once

#include <persistence/state/session_options.hpp>
#include <frontend/session_snapshot.hpp>
#include <ids/ids.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/dom/element.hpp>

#include <nlohmann/json.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Persistence
{
    class StateHolder;
}
class FrontendSessionManager;
class ConfirmDialog;
class TerminalPanel;
class FileExplorerPanel;
class OperationQueue;
class FileTrackingPanel;
class SessionOptions;

/**
 * @brief Orchestrates Session's Lumino layout: the "+" tab-add context menu,
 *        the ten panel factory/deleter closures, the DOM-host attach logic,
 *        and the layout-round-trip for saved / reconnect-resumed sessions.
 *        Also holds the "is this panel currently mounted" tracking for each
 *        single-instance panel (file explorer / operation queue / file
 *        tracking / session options) so rebuildTabAddMenu can flip menu
 *        entries between disabled and enabled.  Extracted from Session as
 *        part of the session.cpp refactor (Step 6).
 */
class SessionLayoutInitializer
{
  public:
    /**
     * @brief Wiring for SessionLayoutInitializer.  Heaviest Params struct in
     *        the session_components family this class sits at the top of
     *        the widget stack and needs pointers to every panel it mounts.
     */
    struct Params
    {
        Persistence::StateHolder* stateHolder = nullptr;
        ConfirmDialog* confirmDialog = nullptr;

        std::string sessionLayoutId;
        std::optional<std::string> layoutName;

        /** @brief Non-owning read for engine-type discrimination and saved layouts map. */
        Persistence::SessionOptions const* engineOptions = nullptr;

        /** @brief Non-owning reference to the Session's FSM observable. */
        Nui::Observed<std::unique_ptr<FrontendSessionManager>>* frontendSessionManager = nullptr;

        /** @brief Drives the per-panel view blocker on operation-queue / file-tracking factories. */
        Nui::Observed<bool>* isInLostConnectionState = nullptr;

        /** @brief Non-owning pointers to the panels the layout mounts. */
        TerminalPanel* terminalPanel = nullptr;
        FileExplorerPanel* fileExplorerPanel = nullptr;
        OperationQueue* operationQueue = nullptr;
        FileTrackingPanel* fileTrackingPanel = nullptr;
        SessionOptions* sessionOptions = nullptr;

        /**
         * @brief Local-shell adoptions queued for consumption by
         *        localShellFactory on reconnect.  Non-owning pointer so the
         *        factory can match & erase entries in place.  Stays on
         *        Session::Implementation until Step 7 (SessionSnapshotManager).
         */
        std::vector<LocalShellAdoption>* pendingLocalShellAdoptions = nullptr;

        /**
         * @brief Called from within the factory to learn the preferred
         *        layout for this initialization: the reconnect-snapshot
         *        layout (if any), otherwise the saved layout named by
         *        `layoutName`.  Returning nullopt means "let Lumino pick
         *        the default blank layout".  The callback is expected to
         *        consume any one-shot state so subsequent calls return
         *        nullopt.
         */
        std::function<std::optional<nlohmann::json>()> takeResumeLayout;

        /** @brief Called when `contentPanelManager.addPanel(...)` returns false. */
        std::function<void()> onLayoutCreationFailed;
    };

    explicit SessionLayoutInitializer(Params params);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SessionLayoutInitializer);

    /**
     * @brief Called from Session's operator()() when the DOM host attaches.
     *        Latches the host weak_ptr; if initialize() was already attempted
     *        before the host was available, replays it now.
     */
    void attachLayoutHost(std::weak_ptr<Nui::Dom::BasicElement> host);

    /**
     * @brief Drives the full layout bring-up: picks a layout, wires all ten
     *        Lumino factory/deleter closures, and calls addPanel.  Safe to
     *        call before the DOM host attaches (it'll defer itself).
     */
    void initialize();

    /** @brief Serialize the current Lumino layout plus file-grid flavor extras. */
    std::optional<nlohmann::json> getLayout() const;

    /**
     * @brief Prepare to spawn a local-shell panel for the named saved shell.
     *        Routes the request through contentPanelManager; the actual
     *        process spawn happens when localShellFactory fires.
     */
    void openLocalShellChannel(std::string const& shellName);

    /** @brief Renders the "+" tab-add popup menu for Session::operator()(). */
    Nui::ElementRenderer tabAddMenuRenderer();

  private:
    struct Implementation;
    static void rebuildTabAddMenuInto(Implementation& impl);
    std::unique_ptr<Implementation> impl_;
};
