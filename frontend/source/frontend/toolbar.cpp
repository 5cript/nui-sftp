#include <frontend/toolbar.hpp>
#include <frontend/classes.hpp>
#include <frontend/session_area.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <frontend/components/icon_panel.hpp>
#include <frontend/events/frontend_events.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>
#include <events/app_event_context.hpp>
#include <constants/layouts.hpp>

#include <script-nui-components/select.hpp>
#include <script-nui-components/button.hpp>

#include <frontend/svgs/add.hpp>
#include <frontend/svgs/decline.hpp>
#include <frontend/svgs/settings.hpp>
#include <ui5-sap-icons/icons/light-mode.hpp>
#include <ui5-sap-icons/icons/dark-mode.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

struct Toolbar::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    SessionArea* sessionArea;
    ConfirmDialog* confirmDialog;
    ThemeController* themeController;
    Nui::Observed<std::string> activeTerminalEngine{};
    Nui::Observed<std::string> selectedLayout{};
    Nui::Observed<std::vector<std::string>> terminalEngines;
    Nui::Observed<std::vector<std::string>> layouts;

    Nui::ListenRemover<decltype(FrontendEvents::onSettingsChanged)> settingsChangedListener;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        ThemeController& themeController
    )
        : stateHolder{stateHolder}
        , events{events}
        , confirmDialog{confirmDialog}
        , themeController{&themeController}
        , terminalEngines{}
        , layouts{}
    {
        Log::info("Toolbar::Implementation()");
    }

    void updateSessionsList(std::function<void()> onDone);
};

void Toolbar::Implementation::updateSessionsList(std::function<void()> onDone)
{
    loadState(
        *stateHolder,
        confirmDialog,
        [this, onDone = std::move(onDone)](bool success, Persistence::State const& state)
        {
            if (!success)
                return;

            std::vector<std::pair<std::string, std::string /*orderby*/>> enginesUnordered;
            for (auto const& [name, engine] : state.sessions)
                enginesUnordered.push_back({name, engine.orderBy.value_or(name)});

            std::sort(
                enginesUnordered.begin(),
                enginesUnordered.end(),
                [](auto const& lhs, auto const& rhs)
                {
                    return lhs.second < rhs.second;
                }
            );

            std::vector<std::string> engines;
            for (auto const& [name, _] : enginesUnordered)
                engines.push_back(name);

            {
                Log::info("Updating terminal engines list.");
                auto proxy = terminalEngines.modify();
                terminalEngines = std::move(engines);
            }
            if (!terminalEngines.value().empty())
            {
                activeTerminalEngine = terminalEngines.value().front();
            }
            Nui::globalEventContext.executeActiveEventsImmediately();
            onDone();
        }
    );
}

Toolbar::Toolbar(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    ConfirmDialog* confirmDialog,
    ThemeController& themeController
)
    : impl_(std::make_unique<Implementation>(stateHolder, events, confirmDialog, themeController))
{
    Log::info("Toolbar::Toolbar");
    impl_->updateSessionsList(
        [this]()
        {
            reloadLayouts();
        }
    );

    impl_->settingsChangedListener = Nui::smartListen(
        impl_->events->onSettingsChanged,
        [this](auto const&)
        {
            impl_->updateSessionsList(
                [this]()
                {
                    reloadLayouts();
                }
            );
        }
    );
}

void Toolbar::connectLayoutsChanged()
{
    listen(
        impl_->events->onLayoutsChanged,
        [this](bool)
        {
            loadState(
                *impl_->stateHolder,
                impl_->confirmDialog,
                [this](bool success, Persistence::State const&)
                {
                    if (!success)
                        return;

                    reloadLayouts();
                },
                language->get("toolbar", "cannotUpdateLayouts")
            );
        }
    );
}

void Toolbar::reloadLayouts()
{
    impl_->selectedLayout = "";
    impl_->layouts.value().clear();
    impl_->layouts.value().push_back(std::string{Constants::defaultLayoutName});
    if (const auto iter = impl_->stateHolder->stateCache().sessions.find(impl_->activeTerminalEngine.value());
        iter != impl_->stateHolder->stateCache().sessions.end())
    {
        if (iter->second.layouts)
        {
            for (auto const& [name, layout] : iter->second.layouts.value())
                impl_->layouts.value().push_back(name);

            if (!impl_->layouts.value().empty())
                impl_->selectedLayout = impl_->layouts.value().front();

            impl_->layouts.modifyNow();
        }
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Toolbar);

Nui::ElementRenderer Toolbar::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return div{class_ = "toolbar"}(
        iconPanel({
            .icon = [](){
                return img{
                    src = "nui://app.example/nui-sftp-logo.svg",
                    style = "width: 32px; height: 32px;",
                }();
            }(),
            .color = "var(--theme-color)",
            .padding = 0,
            .withBorder = false
        }),
        Snc::select(Snc::SelectOptions<decltype(impl_->activeTerminalEngine), decltype(impl_->terminalEngines)>{
            .activeOption = impl_->activeTerminalEngine,
            .options = impl_->terminalEngines,
            .onChange = [this](auto const&, Nui::WebApi::MouseEvent const&) {
                reloadLayouts();
            },
            .makeId = [](){
                return Nui::val::global("generateId")().as<std::string>();
            }
        }),
        Snc::select(Snc::SelectOptions<decltype(impl_->selectedLayout), decltype(impl_->layouts)>{
            .activeOption = impl_->selectedLayout,
            .options = impl_->layouts,
            .attributes = {
                !(reference.onMaterialize([this](auto){
                    connectLayoutsChanged();
                }))
            },
            .makeId = [](){
                return Nui::val::global("generateId")().as<std::string>();
            }
        }),
        Snc::button({
            .text = language->get("toolbar", "newSession"),
            .icon = GeneratedSvgs::add(),
            .attributes = {
                onClick = [this]() {
                    impl_->events->onNewSession = impl_->activeTerminalEngine.value();
                    impl_->events->onNewSession.modifyNow();
                },
            },
        }),
        Snc::button({
            .text = language->get("toolbar", "endSession"),
            .icon = GeneratedSvgs::decline(),
            .attributes = {
                onClick = [this]() {
                    if (impl_->sessionArea) {
                        impl_->sessionArea->removeActiveSession();
                    } else {
                        Log::error("Toolbar::EndSession: sessionArea is not set.");
                    }
                },
            },
        }),
        // spacer:
        div{
            style = "flex-grow: 1"
        }(),
        Snc::button({
            .icon = [this]() -> Nui::ElementRenderer {
                return Nui::Elements::fragment(
                    observe(impl_->events->darkLightMode),
                    [this](){
                        auto mode = impl_->events->darkLightMode.value();
                        if (mode == SharedData::DarkLightMode::System)
                            mode = impl_->themeController->getPreferredMode();

                        if (mode == SharedData::DarkLightMode::Dark)
                            return Ui5Icons::light_mode();
                        return Ui5Icons::dark_mode();
                    }
                );
            }(),
            .attributes = {
                onClick = [this]() {
                    if (impl_->events->darkLightMode.value() == SharedData::DarkLightMode::Dark) {
                        impl_->events->darkLightMode = SharedData::DarkLightMode::Light;
                    } else if (impl_->events->darkLightMode.value() == SharedData::DarkLightMode::Light) {
                        impl_->events->darkLightMode = SharedData::DarkLightMode::Dark;
                    } else {
                        const auto preferred = impl_->themeController->getPreferredMode();
                        impl_->events->darkLightMode = preferred == SharedData::DarkLightMode::Dark ? SharedData::DarkLightMode::Light : SharedData::DarkLightMode::Dark;
                    }
                    // reload theme
                    impl_->events->selectedTheme.modify();

                    impl_->stateHolder->stateCache().uiOptions.darkLightMode = impl_->events->darkLightMode.value();
                    impl_->stateHolder->save(
                        [this](std::optional<std::string> const& error)
                        {
                            if (error)
                            {
                                impl_->confirmDialog->open({
                                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                    .headerText = language->get("toolbar", "errorSavingSettingsHeader"),
                                    .text = fmt::format(
                                        fmt::runtime(language->get("toolbar", "errorSavingSettings") + ": {}"), *error
                                    ),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                            }
                        }
                    );
                },
            },
        }),
        Snc::button({
            .icon = GeneratedSvgs::settings(),
            .attributes = {
                onClick = [this]() {
                    impl_->events->settingsOpen = true;
                },
            },
        })
    );
    // clang-format on
}

std::string Toolbar::selectedLayout() const
{
    return impl_->selectedLayout.value();
}

void Toolbar::sessionArea(SessionArea& sessionArea)
{
    impl_->sessionArea = &sessionArea;
}