#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/events/frontend_events.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

class MainPage
{
  public:
    MainPage(Persistence::StateHolder* stateHolder, FrontendEvents* events);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(MainPage);

    Nui::ElementRenderer render();
    void onSetupComplete();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};