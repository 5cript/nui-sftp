#pragma once

#include <frontend/session.hpp>
#include <frontend/toolbar.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
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
        Toolbar* toolbar
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SessionArea);

    Nui::ElementRenderer operator()();

    void addSession(std::string const& name);
    void registerRpc();
    void removeSession(int index);
    void setSelected(int newIndex);
    void removeActiveSession();

    Session* getActiveSession();
    std::optional<nlohmann::json> getActiveSessionLayout();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};