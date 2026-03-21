#include <frontend/session_components/session_options.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <log/log.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <fmt/format.h>

struct SessionOptions::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    std::string persistenceSessionName;
    std::string sessionLayoutId;
    ConfirmDialog* confirmDialog;

    std::string selectedLayout;
    Nui::Observed<std::vector<std::string>> layoutNames;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::string persistenceSessionName,
        std::string sessionLayoutId,
        ConfirmDialog* confirmDialog
    )
        : stateHolder{stateHolder}
        , events{events}
        , persistenceSessionName{std::move(persistenceSessionName)}
        , sessionLayoutId{std::move(sessionLayoutId)}
        , confirmDialog{confirmDialog}
        , selectedLayout{}
    {}
};

void SessionOptions::loadLayoutNames()
{
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this](bool success, Persistence::State const& state)
        {
            if (!success)
                return;

            impl_->layoutNames.value().clear();

            if (const auto iter = state.sessions.find(impl_->persistenceSessionName);
                iter != end(state.sessions) && iter->second.layouts)
            {
                for (const auto& [name, session] : *iter->second.layouts)
                {
                    impl_->layoutNames.value().push_back(name);
                }
            }

            impl_->layoutNames.modifyNow();
        },
        "Cannot load layout names."
    );
}

SessionOptions::SessionOptions(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    std::string persistenceSessionName,
    std::string sessionLayoutId,
    ConfirmDialog* confirmDialog
)
    : impl_{std::make_unique<Implementation>(
          stateHolder,
          events,
          std::move(persistenceSessionName),
          std::move(sessionLayoutId),
          confirmDialog
      )}
{
    loadLayoutNames();
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(SessionOptions);

Nui::ElementRenderer SessionOptions::operator()()
{
    using Nui::Elements::div; // because of the global div.
    using namespace Nui::Attributes;

    return div{
        style = "width: 100%; height: auto; display: block; padding: 10px",
    }();

    // clang-format off
    // clang-format on
}