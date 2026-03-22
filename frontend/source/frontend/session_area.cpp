#include <frontend/session_area.hpp>
#include <frontend/session.hpp>
#include <frontend/classes.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <log/log.hpp>
#include <events/app_event_context.hpp>

#include <script-nui-components/tabs.hpp>

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
    Nui::RpcClient::AutoUnregister dropHandlerUnregister_;
    ScriptNuiComponents::Tabs tabs;

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
        , dropHandlerUnregister_{}
        , tabs{}
    {}
};

void SessionArea::removeActiveSession()
{
    removeSession(impl_->tabs.selectedId());
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
    impl_->tabs.onClose(
        [this](int id)
        {
            // Confirm Dialog?
            removeSession(id);
            return false;
        }
    );

    impl_->tabs.onSelect(
        [this](int tabId) -> bool
        {
            Nui::WebApi::Console::log("Tab with id '{}' selected.", tabId);
            setSelected(tabId);
            return false;
        }
    );

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
            int tabId = -1;
            for (auto const& session : impl_->sessions.value())
            {
                if (session->getProcessIdIfExecutingEngine().value_or("") == processId)
                {
                    Log::info("Process with id '{}' found in session '{}'.", processId, session->name());
                    tabId = session->tabId();
                    break;
                }
                ++index;
            }

            if (index < impl_->sessions.size())
                removeSession(tabId);
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

void SessionArea::removeSession(int tabId)
{
    const auto indexOpt = findSessionIndexByTabId(tabId);
    if (!indexOpt)
    {
        Log::error("Session index out of bounds: {}", tabId);
        return;
    }
    const auto index = *indexOpt;

    Log::info("Removing session: {}", impl_->sessions.value()[index]->name());

    if (impl_->sessions.value()[index]->visible() && impl_->sessions.size() > 1)
        setSelected(impl_->tabs.firstTabId());

    impl_->sessions.value()[index]->shutdown(
        [this, index, tabId]()
        {
            impl_->sessions.erase(impl_->sessions.begin() + index);
            impl_->tabs.remove(tabId);
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void SessionArea::setSelected(int tabId)
{
    const int previousSelected = impl_->tabs.selectedId();
    auto sync = Nui::ScopeExit{[]() noexcept
        {
            Nui::globalEventContext.sync();
        }};

    if (previousSelected != -1)
    {
        const auto previousSessionIndex = findSessionIndexByTabId(previousSelected);
        if (previousSessionIndex)
            impl_->sessions.value()[*previousSessionIndex]->visible(false);
    }
    impl_->tabs.select(tabId);
    const auto newIndex = findSessionIndexByTabId(tabId);
    if (!newIndex)
    {
        Log::error("Tab with id '{}' not found for selection.", tabId);
        return;
    }
    Log::info("Selected session: {}", *newIndex);
    impl_->sessions.value()[*newIndex]->visible(true);
}

std::optional<std::size_t> SessionArea::findSessionIndexByTabId(int tabId) const
{
    for (size_t i = 0; i != impl_->sessions.size(); ++i)
    {
        if (impl_->sessions.value()[i]->tabId() == tabId)
            return i;
    }
    return std::nullopt;
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
            {
                Log::error("Failed to load state while adding session '{}'", name);
                return;
            }

            auto iter = state.sessions.find(name);
            if (iter == end(state.sessions))
            {
                Log::error("No engine found for name: {}", name);
                return;
            }

            auto [engineKey, engine] = *iter;

            const auto tabId = impl_->tabs.add(name, true);
            Log::info("Adding session: {} with layout {}. Tab ID: {}", name, impl_->toolbar->selectedLayout(), tabId);

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
                        removeSession(ptr->tabId());
                    },
                    [this](Session const* ptr, std::string const& desiredTitle) -> std::string
                    {
                        std::string disambiguatedTitle = desiredTitle;
                        int suffix = 1;
                        bool titleExists = false;

                        do
                        {
                            titleExists = false;
                            for (auto const& session : impl_->sessions.value())
                            {
                                if (auto tabTitle = session->tabTitle().lock();
                                    tabTitle && tabTitle->value() == disambiguatedTitle)
                                {
                                    titleExists = true;
                                    disambiguatedTitle = desiredTitle + " ("s + std::to_string(suffix) + ")"s;
                                    ++suffix;
                                    break;
                                }
                            }
                        } while (titleExists);

                        impl_->tabs.modifyTabById(
                            ptr->tabId(),
                            [&disambiguatedTitle](ScriptNuiComponents::Tabs::Tab* tab)
                            {
                                if (!tab)
                                {
                                    Log::error("Tab not found for modifying title.");
                                    return;
                                }
                                tab->title = disambiguatedTitle;
                            }
                        );
                        return disambiguatedTitle;
                    },
                    impl_->sessions.size() == 0,
                    tabId
                )
            );

            setSelected(tabId);
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
    const auto selected = impl_->tabs.selectedId();
    if (selected == -1)
        return nullptr;

    for (auto const& session : impl_->sessions.value())
    {
        if (session->tabId() == selected)
            return session.get();
    }
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
            impl_->tabs({
                style =
                    "margin-bottom: 2px;"
                    "box-shadow: 1px 4px 10px 0px rgba(0,0,0,1);"
                    "min-height: 30px;"
                    "box-sizing: content-box;"
                    "padding: 3px 5px;"
            }),
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