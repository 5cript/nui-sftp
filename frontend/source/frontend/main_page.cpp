
#include <frontend/main_page.hpp>
#include <frontend/sidebar.hpp>
#include <frontend/toolbar.hpp>
#include <frontend/classes.hpp>
#include <frontend/session_area.hpp>
#include <frontend/settings.hpp>
#include <frontend/licenses.hpp>
#include <utility/language.hpp>
#include <frontend/dialog/password_prompter.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/dialog/direct_connect_dialog.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/onboarding/onboarding.hpp>
#include <frontend/command_store/command_store_client.hpp>
#include <log/log.hpp>

#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

struct MainPage::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    ConfirmDialog confirmDialog;
    InputDialog newItemAskDialog;
    FilePropertyDialog filePropertyDialog;
    ArchiveTransferDialog archiveTransferDialog;
    DirectConnectDialog directConnectDialog;
    NewSessionDialog newSessionDialog;
    PasswordPrompter prompter;
    MultiInputDialog multiInputDialog;
    Sidebar sidebar;
    Toolbar toolbar;
    /// One store client for the whole process; every session records into it and both command panels
    /// read from it.
    CommandStoreClient commandStoreClient;
    SessionArea sessionArea;
    Settings settings;
    Licenses licenses;
    Frontend::Onboarding onboarding;
    Nui::Observed<bool> darkMode;
    Nui::TimerHandle setupWait;

    Implementation(Persistence::StateHolder* stateHolder, FrontendEvents* events, ThemeController& themeController)
        : stateHolder{stateHolder}
        , events{events}
        , confirmDialog{"ConfirmDialog", *stateHolder}
        , newItemAskDialog{"AskDialog"}
        , filePropertyDialog{"FilePropertyDialog"}
        , archiveTransferDialog{"ArchiveTransferDialog"}
        , directConnectDialog{"DirectConnectDialog", stateHolder}
        , newSessionDialog{"NewSessionDialog"}
        , prompter{}
        , multiInputDialog{"MultiInputDialog"}
        , sidebar{stateHolder, events}
        , toolbar{stateHolder, events, &confirmDialog, &directConnectDialog, themeController}
        , commandStoreClient{}
        , sessionArea{stateHolder, events, &newItemAskDialog, &confirmDialog, &filePropertyDialog, &archiveTransferDialog, &toolbar, &commandStoreClient}
        , settings{stateHolder, events, [this](){
            return sessionArea.getActiveSessionLayout();
        }, newItemAskDialog, confirmDialog, multiInputDialog, newSessionDialog}
        , licenses{events}
        , onboarding{stateHolder, events}
        , darkMode{true}
        , setupWait{}
    {
        Log::info("MainPage::Implementation()");
        toolbar.sessionArea(sessionArea);
        toolbar.settings(settings);
    }

    ~Implementation()
    {
        Log::info("MainPage::~Implementation()");
    }
};

MainPage::MainPage(Persistence::StateHolder* stateHolder, FrontendEvents* events, ThemeController& themeController)
    : impl_{std::make_unique<Implementation>(stateHolder, events, themeController)}
{
    Log::info("MainPage::MainPage()");
}

void MainPage::onSetupComplete()
{
    Log::info("Setup is complete.");
    Nui::RpcClient::callWithBackChannel(
        "Main::getInitialPersistenceLoadWarning",
        [this](Nui::val response)
        {
            auto showPersistenceWarning = [this, response]()
            {
                if (!response.hasOwnProperty("warning"))
                {
                    impl_->onboarding.maybeStart();
                    return;
                }
                const auto warning = response["warning"].as<std::string>();
                if (warning.empty())
                {
                    impl_->onboarding.maybeStart();
                    return;
                }
                if (!impl_->stateHolder->stateCache().uiOptions.onboardingCompleted)
                {
                    impl_->onboarding.maybeStart();
                    return;
                }
                impl_->confirmDialog.open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                    .headerText = language->get("persistence", "warningLoadingState"),
                    .text = fmt::format(fmt::runtime(language->get("persistence", "loadedWithWarnings")), warning),
                    .buttons = ConfirmDialog::Button::Ok,
                    .neverShowAgainId = "persistenceLoadWarning",
                    .onClose = [this](auto)
                    {
                        impl_->onboarding.maybeStart();
                    },
                });
            };

            if (response.hasOwnProperty("isRoot") && response["isRoot"].as<bool>())
            {
                impl_->confirmDialog.open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("rootWarning", "header"),
                    .text = language->get("rootWarning", "text"),
                    .buttons = ConfirmDialog::Button::Ok,
                    .onClose = [showPersistenceWarning](auto)
                    {
                        showPersistenceWarning();
                    },
                });
                return;
            }

            showPersistenceWarning();
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(MainPage);

Nui::ElementRenderer MainPage::render()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.

    Log::info("MainPage::render()");
    Nui::ScopeExit onLeaveRender(
        []() noexcept
        {
            Log::info("MainPage::render() complete");
        }
    );

    // clang-format off
    return div{
        class_ = "main-page-wrap"
    }(
        impl_->newItemAskDialog(),
        impl_->prompter.dialog(),
        impl_->confirmDialog(),
        impl_->filePropertyDialog(),
        impl_->archiveTransferDialog(),
        impl_->directConnectDialog(),
        impl_->newSessionDialog(),
        impl_->multiInputDialog(),
        impl_->settings(),
        impl_->licenses(),
        div{
            class_ = "main-page",
        }(
            impl_->toolbar(),
            impl_->sessionArea()
        )
    );
    // clang-format on
}