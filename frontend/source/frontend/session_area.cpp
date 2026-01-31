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
    FilePropertyDialog* filePropertyDialog;
    Toolbar* toolbar;
    Nui::Observed<std::vector<std::unique_ptr<Session>>> sessions;
    Nui::Observed<std::optional<int>> selected;
    Nui::RpcClient::AutoUnregister dropHandlerUnregister_;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* newItemAskDialog,
        ConfirmDialog* confirmDialog,
        FilePropertyDialog* filePropertyDialog,
        Toolbar* toolbar
    )
        : stateHolder{stateHolder}
        , events{events}
        , newItemAskDialog{newItemAskDialog}
        , confirmDialog{confirmDialog}
        , filePropertyDialog{filePropertyDialog}
        , toolbar{toolbar}
        , sessions{}
        , selected{std::nullopt}
        , dropHandlerUnregister_{}
    {}
};

void SessionArea::removeActiveSession()
{
    if (!impl_->selected->has_value())
    {
        Log::error("SessionArea::removeActiveSession: No active session to remove (selected is nullopt)");
        return;
    }

    removeSession(impl_->selected->value());
}

SessionArea::SessionArea(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    InputDialog* newItemAskDialog,
    ConfirmDialog* confirmDialog,
    FilePropertyDialog* filePropertyDialog,
    Toolbar* toolbar
)
    : impl_{std::make_unique<
          Implementation>(stateHolder, events, newItemAskDialog, confirmDialog, filePropertyDialog, toolbar)}
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

    impl_->dropHandlerUnregister_ = Nui::RpcClient::autoRegisterFunction(
        "SessionArea::onFilesDropped",
        [this](Nui::val val)
        {
            const auto json = nlohmann::json::parse(Nui::JSON::stringify(val));
            const auto dropMetadata = json.value("dropMetadata", "");
            const auto isLeft = json.value("isLeft", false);

            Session* session = getSessionByLayoutId(dropMetadata);
            if (!session)
            {
                Log::error("No session found for dropMetadata '{}'", dropMetadata);
                return;
            }

            std::optional<std::string> subdir{std::nullopt};
            if (json.contains("subdir"))
                subdir = json["subdir"].get<std::string>();

            session->onDrop(
                isLeft,
                json.value("entries", nlohmann::json::array()).get<std::vector<SharedData::DirectoryEntry>>(),
                subdir
            );
        }
    );
}

void SessionArea::removeSession(int index)
{
    if (index < 0 || index >= static_cast<int>(impl_->sessions.size()))
    {
        Log::critical("Session index out of bounds: {}", index);
        return;
    }

    Log::info("Removing session: {}", impl_->sessions.value()[index]->name());

    if (impl_->sessions.value()[index]->visible() && impl_->sessions.size() > 1)
        setSelected(0);

    impl_->sessions.value()[index]->shutdown(
        [this, index]()
        {
            impl_->sessions.erase(impl_->sessions.begin() + index);
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void SessionArea::setSelected(int newIndex)
{
    [this, newIndex]()
    {
        if (!impl_->selected->has_value())
        {
            Log::info("Setting selected session to index: {}", newIndex);
            impl_->sessions.value()[newIndex]->visible(true);
            impl_->selected = newIndex;
            return;
        }

        const auto oldIndex = impl_->selected->value();
        if (oldIndex >= 0 && oldIndex < static_cast<int>(impl_->sessions.size()))
        {
            Log::info("Changing selected session from index '{}' to index '{}'", oldIndex, newIndex);
            impl_->sessions.value()[oldIndex]->visible(false);
            impl_->sessions.value()[newIndex]->visible(true);
            impl_->selected = newIndex;
            return;
        }
        return;
    }();

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
                    impl_->filePropertyDialog,
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
                    [this](std::string const& desiredTitle) -> std::string
                    {
                        std::string disambiguatedTitle = desiredTitle;
                        int suffix = 1;
                        bool titleExists = false;

                        do
                        {
                            titleExists = false;
                            for (auto const& session : impl_->sessions.value())
                            {
                                if (session->name() == disambiguatedTitle)
                                {
                                    titleExists = true;
                                    disambiguatedTitle = desiredTitle + " ("s + std::to_string(suffix) + ")"s;
                                    ++suffix;
                                    break;
                                }
                            }
                        } while (titleExists);

                        return disambiguatedTitle;
                    },
                    impl_->sessions.size() == 0
                )
            );

            setSelected(static_cast<int>(impl_->sessions.size()) - 1);
        },
        "Cannot add session."
    );
}

std::optional<nlohmann::json> SessionArea::getActiveSessionLayout()
{
    Session* activeSession = getActiveSession();
    if (activeSession)
        return activeSession->getLayout();
    return std::nullopt;
}

Session* SessionArea::getActiveSession()
{
    if (impl_->selected.value() && *impl_->selected.value() >= 0 &&
        *impl_->selected.value() < static_cast<int>(impl_->sessions.size()))
        return impl_->sessions.value()[*impl_->selected.value()].get();
    return nullptr;
}

Session* SessionArea::getSessionByLayoutId(std::string const& layoutId)
{
    for (auto const& session : impl_->sessions.value())
    {
        if (session->layoutId() == layoutId)
            return session.get();
    }
    return nullptr;
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
                    setSelected(index);
                },
                "fixed"_prop = true
            }(
                range(impl_->sessions),
                [this](long long i, auto& session) -> Nui::ElementRenderer {
                    // tabs dont actually reside here:
                    return ui5::tab{
                        "text"_prop = session->tabTitle(),
                        "selected"_prop = observe(impl_->selected).generate([i](std::optional<int> index){
                            if (index.has_value())
                                return *index == i;
                            return false;
                        }),
                        "moveable"_prop = true
                    }();
                }
            ),
            div{
                style = "position: relative; width: 100%; height: calc(100% - 30px); display: block",
                class_ = "session-area-content"
            }(
                range(impl_->sessions),
                [](long long, auto& session) -> Nui::ElementRenderer {
                    return (*session)();
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