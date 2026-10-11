#include <frontend/toolbar.hpp>
#include <frontend/classes.hpp>
#include <frontend/session_area.hpp>
#include <frontend/settings.hpp>
#include <frontend/notification_center.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/direct_connect_dialog.hpp>
#include <frontend/settings/ssh_user_settings.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>
#include <events/app_event_context.hpp>
#include <constants/layouts.hpp>

#include <script-nui-components/select.hpp>
#include <script-nui-components/button.hpp>
#include <script-nui-components/style_variant.hpp>

#include <frontend/onboarding/onboarding.hpp>

#include <frontend/brand_mark.hpp>
#include <frontend/svgs/add.hpp>
#include <frontend/svgs/decline.hpp>
#include <frontend/svgs/settings.hpp>
#include <ui5-sap-icons/icons/light-mode.hpp>
#include <ui5-sap-icons/icons/dark-mode.hpp>
#include <ui5-sap-icons/icons/connected.hpp>
#include <ui5-sap-icons/icons/begin.hpp>
#include <ui5-sap-icons/icons/signature.hpp>
#include <ui5-sap-icons/icons/bell.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

#include <numeric>

struct Toolbar::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    SessionArea* sessionArea;
    Settings* settings{nullptr};
    NotificationCenter* notificationCenter{nullptr};
    ConfirmDialog* confirmDialog;
    DirectConnectDialog* directConnectDialog;
    ThemeController* themeController;
    Nui::Observed<std::string> activeTerminalEngine{};
    Nui::Observed<std::string> selectedLayout{};
    Nui::Observed<std::vector<std::string>> terminalEngines;
    Nui::Observed<std::vector<std::string>> layouts;
    Nui::Observed<std::size_t> selectedUser{0u};
    Nui::Observed<std::vector<std::size_t>> userIndices;
    std::vector<std::string> userLabels;

    Nui::ListenRemover<decltype(FrontendEvents::onSettingsChanged)> settingsChangedListener;
    Nui::ListenRemover<decltype(FrontendEvents::onLayoutsChanged)> layoutsChangedListener;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        DirectConnectDialog* directConnectDialog,
        ThemeController& themeController
    )
        : stateHolder{stateHolder}
        , events{events}
        , confirmDialog{confirmDialog}
        , directConnectDialog{directConnectDialog}
        , themeController{&themeController}
        , terminalEngines{}
        , layouts{}
    {
        Log::info("Toolbar::Implementation()");
    }

    void updateSessionsList(std::function<void()> onDone);
    void reloadUsers();
    Nui::ElementRenderer notificationsButton();
    Nui::ElementRenderer userSelect();
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
    DirectConnectDialog* directConnectDialog,
    ThemeController& themeController
)
    : impl_(std::make_unique<Implementation>(stateHolder, events, confirmDialog, directConnectDialog, themeController))
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
    impl_->layoutsChangedListener = Nui::smartListen(
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

void Toolbar::Implementation::reloadUsers()
{
    userLabels.clear();
    if (const auto iter = stateHolder->stateCache().sessions.find(activeTerminalEngine.value());
        iter != stateHolder->stateCache().sessions.end())
    {
        if (const auto* ssh = std::get_if<Persistence::SshSessionOptions>(&iter->second.engine))
        {
            for (auto const& user : ssh->users)
                userLabels.push_back(sshUserLabel(user));
        }
    }

    std::vector<std::size_t> indices(userLabels.size());
    std::iota(indices.begin(), indices.end(), 0u);
    selectedUser = 0u;
    userIndices = std::move(indices);
}

Nui::ElementRenderer Toolbar::Implementation::userSelect()
{
    using namespace Nui::Attributes;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return Snc::select(Snc::SelectOptions<decltype(selectedUser), decltype(userIndices)>{
        .activeOption = selectedUser,
        .options = userIndices,
        .attributes = {
            title = language->get("toolbar", "userSelect"),
            style = Nui::observe(userIndices).generate([](std::vector<std::size_t> const& indices) -> std::string {
                return indices.size() < 2u ? "display: none;" : "";
            }),
        },
        .activeRenderer = [this](auto const&) -> Nui::ElementRenderer {
            return span{}(Nui::observe(selectedUser, userIndices), [this]() -> std::string {
                if (selectedUser.value() >= userLabels.size())
                    return "";
                return userLabels[selectedUser.value()];
            });
        },
        .elementRenderer = [this](std::size_t index) -> Nui::ElementRenderer {
            if (index >= userLabels.size())
                return Nui::nil();
            return span{}(userLabels[index]);
        },
        .makeId = []() {
            return Nui::val::global("generateId")().as<std::string>();
        },
    });
    // clang-format on
}

void Toolbar::reloadLayouts()
{
    impl_->reloadUsers();

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

Nui::ElementRenderer Toolbar::Implementation::notificationsButton()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    if (!notificationCenter)
    {
        Log::error("Toolbar: notificationCenter is not set.");
        return Nui::nil();
    }

    const auto severityName = [](NotificationSeverity severity) -> std::string {
        return severity == NotificationSeverity::Error ? "error" : "warning";
    };

    // The badge is rendered anew on every change, which replays its pop animation for each arrival.
    // clang-format off
    return span{
        class_ = "toolbar-notifications",
        "data-attention"_attr = observe(notificationCenter->attention()).generate([this, severityName]() -> std::string {
            auto const& attention = notificationCenter->attention().value();
            return attention.unread == 0 ? std::string{} : severityName(attention.worst);
        }),
    }(
        Snc::button({
            .icon = Ui5Icons::bell(),
            .attributes = {
                Nui::Attributes::title = language->get("notificationLog", "title"),
                onClick = [this]() {
                    notificationCenter->open();
                },
            },
        }),
        span{class_ = "toolbar-notifications-badge-host"}(
            observe(notificationCenter->attention()),
            [this, severityName]() -> Nui::ElementRenderer {
                auto const& attention = notificationCenter->attention().value();
                if (attention.unread == 0)
                    return Nui::nil();
                return span{
                    class_ = "toolbar-notifications-badge",
                    "data-severity"_attr = severityName(attention.worst),
                }(attention.unread > 99 ? std::string{"99+"} : std::to_string(attention.unread));
            }
        )
    );
    // clang-format on
}

Nui::ElementRenderer Toolbar::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return div{class_ = "toolbar"}(
        div{class_ = "toolbar-brand-mark"}(
            Frontend::brandMarkSvg()
        ),
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
        impl_->userSelect(),
        Snc::button({
            .text = language->get("toolbar", "connect"),
            .icon = Ui5Icons::begin(),
            .attributes = {
                disabled = Nui::observe(impl_->terminalEngines).generate(
                    [](auto const& engines) -> std::optional<bool> {
                        return engines.empty() ? std::optional<bool>{true} : std::nullopt;
                    }
                ),
                onClick = [this]() {
                    if (impl_->terminalEngines.value().empty())
                        return;
                    impl_->events->onNewSession = impl_->activeTerminalEngine.value();
                    impl_->events->onNewSession.modifyNow();
                },
            },
            .styleVariant = Nui::observe(impl_->terminalEngines).generate(
                [](auto const& engines) {
                    return engines.empty() ? Snc::StyleVariant::Regular : Snc::StyleVariant::Primary;
                }
            ),
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
        Snc::button({
            .text = language->get("toolbar", "newSession"),
            .icon = GeneratedSvgs::add(),
            .attributes = {
                onClick = [this]() {
                    if (!impl_->settings) {
                        Log::error("Toolbar::AddServer: settings is not set.");
                        return;
                    }
                    impl_->settings->addNewSession();
                },
            },
            .styleVariant = Nui::observe(impl_->terminalEngines).generate(
                [](auto const& engines) {
                    return engines.empty() ? Snc::StyleVariant::Primary : Snc::StyleVariant::Regular;
                }
            ),
        }),
        Snc::button({
            .text = language->get("toolbar", "directConnect"),
            .icon = Ui5Icons::connected(),
            .attributes = {
                onClick = [this]() {
                    if (!impl_->directConnectDialog) {
                        Log::error("Toolbar::DirectConnect: directConnectDialog is not set.");
                        return;
                    }
                    impl_->directConnectDialog->open({
                        .onConfirm = [this](DirectConnectDialog::ConfirmResult const& result) {
                            if (!impl_->sessionArea) {
                                Log::error("Toolbar::DirectConnect: sessionArea is not set.");
                                return;
                            }
                            impl_->sessionArea->addDirectConnectSession(result.sshOptions, result.inheritables);
                        },
                    });
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
                                        fmt::runtime(language->get("toolbar", "errorSavingSettings") + ": {}"), language->translate(*error)
                                    ),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                            }
                        }
                    );
                },
            },
        }),
        impl_->notificationsButton(),
        Snc::button({
            .icon = Ui5Icons::signature(),
            .attributes = {
                Nui::Attributes::title = language->get("licenses", "title"),
                onClick = [this]() {
                    impl_->events->licensesOpen = true;
                },
            },
        }),
        Snc::button({
            .icon = GeneratedSvgs::settings(),
            .attributes = {
                id = std::string{Frontend::OnboardingTargets::toolbarSettingsButtonId},
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

std::size_t Toolbar::selectedUser(std::string const& sessionName) const
{
    if (sessionName != impl_->activeTerminalEngine.value())
        return 0u;
    return impl_->selectedUser.value();
}

void Toolbar::sessionArea(SessionArea& sessionArea)
{
    impl_->sessionArea = &sessionArea;
}

void Toolbar::settings(Settings& settings)
{
    impl_->settings = &settings;
}

void Toolbar::notificationCenter(NotificationCenter& center)
{
    impl_->notificationCenter = &center;
}