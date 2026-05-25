#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/direct_connect_dialog.hpp>
#include <frontend/theme_controller.hpp>
#include <persistence/state_holder.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class SessionArea;
class Settings;

class Toolbar
{
  public:
    Toolbar(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        DirectConnectDialog* directConnectDialog,
        ThemeController& themeController
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Toolbar);

    void sessionArea(SessionArea& sessionArea);
    void settings(Settings& settings);

    Nui::ElementRenderer operator()();
    std::string selectedLayout() const;

  private:
    void connectLayoutsChanged();
    void reloadLayouts();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};