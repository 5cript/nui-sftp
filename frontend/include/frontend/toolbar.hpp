#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <persistence/state_holder.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class SessionArea;

class Toolbar
{
  public:
    Toolbar(Persistence::StateHolder* stateHolder, FrontendEvents* events, ConfirmDialog* confirmDialog);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Toolbar);

    void sessionArea(SessionArea& sessionArea);

    Nui::ElementRenderer operator()();
    std::string selectedLayout() const;

  private:
    void connectLayoutsChanged();
    void reloadLayouts();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};