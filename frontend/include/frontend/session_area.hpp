#pragma once

#include <frontend/session.hpp>
#include <frontend/session_snapshot.hpp>
#include <frontend/toolbar.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <persistence/state_holder.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class SessionArea
{
  public:
    SessionArea(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* newItemAskDialog,
        ConfirmDialog* confirmDialog,
        FilePropertyDialog* filePropertyDialog,
        ArchiveTransferDialog* archiveTransferDialog,
        Toolbar* toolbar,
        CommandStoreClient* commandStoreClient
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SessionArea);

    Nui::ElementRenderer operator()();

    void addSession(std::string const& name);
    void addDirectConnectSession(Persistence::SshSessionOptions const& sshOptions);
    void registerRpc();
    void removeSession(int tabId);
    void setSelected(int tabId);
    void removeActiveSession();

    Session* getActiveSession();
    std::optional<nlohmann::json> getActiveSessionLayout();
    Session* getSessionByLayoutId(std::string const& layoutId);

  private:
    std::optional<std::size_t> findSessionIndexByTabId(int tabId) const;

    /**
     * @brief Reconnect entry point invoked from a Session's requestReconnect
     *        callback and from the internal retry backoff.  Tears down the
     *        Session currently at @p tabId (if any) and schedules / performs
     *        construction of a fresh Session with the saved snapshot applied.
     *        @p attempt bumps across retries; @p snapshot is preserved in
     *        impl_->reconnectStates and re-used on subsequent failures.
     */
    void replaceAtTabId(int tabId, SessionSnapshot snapshot, int attempt);

    /// Starts the per-second countdown tick + schedules the next retry's
    /// replaceAtTabId call after the appropriate exponential backoff.
    void scheduleReconnectRetry(int tabId);

    /// Clears any pending retry timer/countdown for @p tabId.
    void cancelReconnectTimers(int tabId);

    /// User-triggered "Now": drops the scheduled retry timer and fires the
    /// next attempt synchronously so the backoff countdown doesn't waste
    /// the user's time when they already know the network is back.
    void fireReconnectRetryNow(int tabId);

    /// User-triggered Cancel on the in-session reconnect overlay.  Destroys
    /// any in-flight ProtoSession, clears retry timers and the per-tab
    /// ReconnectState, then flips the old Session's overlay back to its
    /// idle [Reconnect] state so the user can re-initiate when ready.
    void abortReconnectCycle(int tabId);

    /// Fires when a candidate ProtoSession's transport open has succeeded.
    /// Consumes the proto, tears down the preserved old Session at @p tabId,
    /// and inserts a fresh adopting Session in its place.
    void onProtoReady(int tabId);

    /// Fires when a candidate ProtoSession's open has failed.  Drops the
    /// proto and arms the next backoff via scheduleReconnectRetry.
    void onProtoFailed(int tabId, std::string const& info);

    /**
     * @brief Builds a Session::Params ready to instantiate.  Centralises the
     *        closeSelf / disambiguateTitle / requestReconnect / onReconnect*
     *        callback wiring that every Session in this area needs.
     */
    Session::Params makeSessionParams(
        Persistence::SessionOptions engineOptions,
        Persistence::UiOptions uiOptions,
        std::string displayName,
        std::optional<std::string> layoutName,
        bool visible,
        int tabId,
        std::optional<SessionSnapshot> resumeFromSnapshot
    );

    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};