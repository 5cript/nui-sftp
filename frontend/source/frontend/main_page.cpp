#include <frontend/main_page.hpp>
#include <frontend/sidebar.hpp>
#include <frontend/toolbar.hpp>
#include <frontend/classes.hpp>
#include <frontend/session_area.hpp>
#include <frontend/settings.hpp>
#include <frontend/dialog/password_prompter.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
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
    PasswordPrompter prompter;
    Sidebar sidebar;
    Toolbar toolbar;
    SessionArea sessionArea;
    Settings settings;
    Nui::Observed<bool> darkMode;
    Nui::TimerHandle setupWait;

    Implementation(Persistence::StateHolder* stateHolder, FrontendEvents* events)
        : stateHolder{stateHolder}
        , events{events}
        , confirmDialog{"ConfirmDialog"}
        , newItemAskDialog{"AskDialog"}
        , prompter{}
        , sidebar{stateHolder, events}
        , toolbar{stateHolder, events, &confirmDialog}
        , sessionArea{stateHolder, events, &newItemAskDialog, &confirmDialog, &toolbar}
        , settings{stateHolder, events, &newItemAskDialog, &confirmDialog}
        , darkMode{true}
        , setupWait{}
    {
        Log::info("MainPage::Implementation()");
        toolbar.sessionArea(sessionArea);
    }

    ~Implementation()
    {
        Log::info("MainPage::~Implementation()");
    }
};

MainPage::MainPage(Persistence::StateHolder* stateHolder, FrontendEvents* events)
    : impl_{std::make_unique<Implementation>(stateHolder, events)}
{
    Log::info("MainPage::MainPage()");
}

void MainPage::onSetupComplete()
{
    Log::info("Setup is complete.");
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(MainPage);

Nui::ElementRenderer MainPage::render()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.

    Log::info("MainPage::render()");

    // clang-format off
    return div{
        class_ = "main-page-wrap"
    }(
        impl_->newItemAskDialog(),
        impl_->prompter.dialog(),
        impl_->confirmDialog(),
        impl_->settings(),
        div{
            style = "background-color: var(--sapBackgroundColor); color: var(--sapTextColor);",
            class_ = "main-page",
        }(
            impl_->toolbar(),
            impl_->sessionArea()
        )
    );
    // clang-format on
}