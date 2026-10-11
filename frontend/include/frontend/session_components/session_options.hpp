#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/events/frontend_events.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <optional>
#include <string>

class SessionOptions
{
  public:
    /**
     * @param persistenceSessionName The name of the session in the settings, nothing for direct connects.
     */
    SessionOptions(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::optional<std::string> persistenceSessionName,
        std::string sessionLayoutId,
        ConfirmDialog* confirmDialog
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SessionOptions);

    Nui::ElementRenderer operator()();

  private:
    void loadLayoutNames();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};