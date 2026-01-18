#include <frontend/session_area.hpp>
#include <frontend/session.hpp>
#include <frontend/classes.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <log/log.hpp>
#include <events/app_event_context.hpp>

#include <ui5/components/tab_container.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/rpc.hpp>

#include <list>
#include <variant>

struct SessionArea::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* newItemAskDialog;
    ConfirmDialog* confirmDialog;
    Toolbar* toolbar;
    Nui::Observed<std::vector<std::unique_ptr<Session>>> sessions;
    int selected;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* newItemAskDialog,
        ConfirmDialog* confirmDialog,
        Toolbar* toolbar
    )
        : stateHolder{stateHolder}
        , events{events}
        , newItemAskDialog{newItemAskDialog}
        , confirmDialog{confirmDialog}
        , toolbar{toolbar}
        , sessions{}
        , selected{0}
    {}
};

void SessionArea::removeActiveSession()
{
    if (impl_->selected >= 0 && impl_->selected < static_cast<int>(impl_->sessions.size()))
    {
        removeSession(impl_->selected);
    }
    else
    {
        Log::error("SessionArea::removeActiveSession: No active session to remove.");
    }
}

SessionArea::SessionArea(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    InputDialog* newItemAskDialog,
    ConfirmDialog* confirmDialog,
    Toolbar* toolbar
)
    : impl_{std::make_unique<Implementation>(stateHolder, events, newItemAskDialog, confirmDialog, toolbar)}
{
    listen(
        events->onNewSession,
        [this](std::string const& name) -> void
        {
            addSession(name);
        }
    );

    loadState(
        *stateHolder,
        impl_->confirmDialog,
        [this](bool success, Persistence::State const& state)
        {
            if (!success)
                return;

            for (auto const& [name, session] : state.sessions)
            {
                if (session.startupSession)
                    addSession(name);
            }
        }
    );

    registerRpc();
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(SessionArea);

void SessionArea::registerRpc()
{
    Nui::RpcClient::registerFunction(
        "SessionArea::processDied",
        [this](Nui::val val)
        {
            auto const processId = val["id"].as<std::string>();
            Log::info("Process with id '{}' terminated.", processId);

            std::size_t index = 0;
            for (auto const& session : impl_->sessions.value())
            {
                if (session->getProcessIdIfExecutingEngine().value_or("") == processId)
                {
                    Log::info("Process with id '{}' found in session '{}'.", processId, session->name());
                    break;
                }
                ++index;
            }

            if (index < impl_->sessions.size())
                removeSession(index);
            else
            {
                Log::error("Process with id '{}' not found in any session.", processId);
            }
        }
    );
}

void SessionArea::removeSession(std::size_t index)
{
    if (index >= impl_->sessions.size())
    {
        Log::critical("Session index out of bounds: {}", index);
        return;
    }

    Log::info("Removing session: {}", impl_->sessions.value()[index]->name());

    if (impl_->sessions.value()[index]->visible() && impl_->sessions.size() > 1)
        setSelected(std::max(0ull, index - 1ull));

    impl_->sessions.value()[index]->shutdown(
        [this, index]()
        {
            impl_->sessions.erase(impl_->sessions.begin() + index);
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void SessionArea::setSelected(int index)
{
    const auto wasAnythingSelected = [this, index]()
    {
        if (impl_->selected >= 0 && impl_->selected < static_cast<int>(impl_->sessions.size()))
        {
            impl_->sessions.value()[impl_->selected]->visible(false);
        }
        if (index >= 0 && index < static_cast<int>(impl_->sessions.size()))
        {
            impl_->sessions.value()[index]->visible(true);
            impl_->selected = index;
            return true;
        }
        return false;
    }();

    if (wasAnythingSelected)
        Nui::globalEventContext.executeActiveEventsImmediately();
}

void SessionArea::addSession(std::string const& name)
{
    using namespace std::string_literals;

    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, name](bool success, Persistence::State const& state)
        {
            if (!success)
                return;

            auto iter = state.sessions.find(name);
            if (iter == end(state.sessions))
            {
                Log::error("No engine found for name: {}", name);
                return;
            }

            auto [engineKey, engine] = *iter;

            Log::info("Adding session: {} with layout {}", name, impl_->toolbar->selectedLayout());
            impl_->sessions.emplace_back(
                std::make_unique<Session>(
                    impl_->stateHolder,
                    impl_->events,
                    engine,
                    state.uiOptions,
                    name,
                    impl_->toolbar->selectedLayout(),
                    impl_->newItemAskDialog,
                    impl_->confirmDialog,
                    [this](Session const* ptr)
                    {
                        auto const index = std::distance(
                            begin(impl_->sessions.value()),
                            std::find_if(
                                begin(impl_->sessions.value()),
                                end(impl_->sessions.value()),
                                [ptr](auto const& session)
                                {
                                    return session.get() == ptr;
                                }
                            )
                        );
                        removeSession(index);
                    },
                    impl_->sessions.size() == 0
                )
            );

            if (impl_->selected >= 0 && impl_->selected < static_cast<int>(impl_->sessions.size()))
                impl_->sessions.value()[impl_->selected]->visible(false);
            impl_->selected = impl_->sessions.size() - 1;
            Nui::globalEventContext.executeActiveEventsImmediately();
        },
        "Cannot add session."
    );
}

Nui::ElementRenderer SessionArea::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.

    Log::info("SessionArea::operator()");
    auto onExit = Nui::ScopeExit(
        []() noexcept
        {
            Log::info("SessionArea::operator() complete");
        }
    );

    try
    {
        // clang-format off
        return div{
            class_ = "session-area"
        }(
            ui5::tabcontainer{
                style = "width: 100%; display: block",
                class_ = "session-area-tabs",
                "tab-select"_event = [this](Nui::val event){
                    const auto index = event["detail"]["tabIndex"].as<int>();
                    if (index >= 0 && index < static_cast<int>(impl_->sessions.size()))
                    {
                        // Could do some logic, but this is easier when tabs are getting deleted.
                        for (auto const& session : impl_->sessions.value())
                        {
                            if (session->visible())
                                session->visible(false);
                        }
                        impl_->sessions.value()[index]->visible(true);
                    }
                },
                "fixed"_prop = true
            }(
                range(impl_->sessions),
                [this](long long i, auto& session) -> Nui::ElementRenderer {
                    // tabs dont actually reside here:
                    return ui5::tab{
                        "text"_prop = session->tabTitle(),
                        "selected"_prop = i == impl_->selected,
                        "moveable"_prop = true
                    }();
                }
            ),
            div{
                style = "position: relative; width: 100%; height: calc(100% - 30px); display: block",
                class_ = "session-area-content"
            }(
                range(impl_->sessions),
                [this](long long i, auto& session) -> Nui::ElementRenderer {
                    return session->operator()(i == impl_->selected);
                }
            )
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in SessionArea::operator(): {}", e.what());
        return div{}("Error loading session area: "s + e.what());
    }
}