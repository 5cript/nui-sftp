#include <frontend/settings.hpp>

#include <frontend/components/icon_panel.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <log/log.hpp>

#include <ui5/components/button.hpp>

#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

struct Settings::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;
    NewSessionDialog newSessionDialog{"settings"};
    Nui::Observed<Settings::Section> activeSection{Settings::Section::GeneralSettings};
    Nui::Observed<std::optional<std::string>> activeSession{};

    Nui::Observed<std::vector<Settings::SectionSelectorOptions>> sessionSelectors{{
        {.sessionId = "Session 1", .icon = "it-system"},
        {.sessionId = "Session 2", .icon = "it-system"},
        {.sessionId = "Session 3", .icon = "it-system"},
    }};

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* inputDialog,
        ConfirmDialog* confirmDialog
    )
        : stateHolder{stateHolder}
        , events{events}
        , inputDialog{inputDialog}
        , confirmDialog{confirmDialog}
    {}
};

Settings::Settings(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    InputDialog* inputDialog,
    ConfirmDialog* confirmDialog
)
    : impl_{std::make_unique<Implementation>(stateHolder, events, inputDialog, confirmDialog)}
{}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Settings);

Nui::ElementRenderer Settings::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.

    return div{
        class_ = "settings-page-background-blocker",
        style = observe(impl_->events->settingsOpen)
            .generate(
                [](bool isOpen) -> std::string
                {
                    return isOpen ? "display: flex;" : "display: none;";
                }
            ),
    }(impl_->newSessionDialog(),
        div{
            class_ = "settings-page",
        }(header(),
            div{
                class_ = "settings-page-content",
            }(side(),
                div{
                    class_ = "settings-page-content-main",
                }("Main Content"))));
}

Nui::ElementRenderer Settings::header()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.

    // clang-format off
    return div{
        class_ = "settings-page-header",
    }(
        iconPanel({
            .name = "action-settings",
            .color = "var(--sapBrandColor)",
            .withBorder = true
        }),
        div{class_ = "title"}("Settings"),
        ui5::button{
            "design"_prop = "Transparent",
            "icon"_prop = "decline",
            "click"_event = [this]() {
                impl_->events->settingsOpen = false;
            },
        }()
    );
    // clang-format on
}

bool Settings::isActive(SectionSelectorOptions const& options)
{
    if (options.thisSection == Section::Session)
        return options.sessionId && *impl_->activeSession == options.sessionId.value_or("");
    else
    {
        return options.thisSection == *impl_->activeSection;
    }
}

void Settings::addNewSession()
{
    impl_->newSessionDialog.open(
        [](auto const& result)
        {
            Log::info("New session created: {} with icon {}", result.sessionName, result.iconName);
        }
    );
}

Nui::ElementRenderer Settings::sectionSelector(SectionSelectorOptions const& options)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.
    using Nui::Elements::span;

    // clang-format off
    return div{
        class_ = observe(impl_->activeSection, impl_->activeSession).generate(
            [this, options]()
            {
                return fmt::format("settings-page-section-selector {}", isActive(options) ? "active" : "");
            }
        ),
        onClick = [this, options]() {
            if (options.thisSection == Section::Add) {
                addNewSession();
                return;
            }

            if (options.thisSection == Section::Session && options.sessionId.has_value()) {
                impl_->activeSection = Section::Session;
                impl_->activeSession = options.sessionId;
            } else {
                impl_->activeSection = options.thisSection;
                impl_->activeSession = std::nullopt;
            }
        },
    }(
        observe(impl_->activeSection, impl_->activeSession),
        [this, options](){
            const auto active = isActive(options);

            return fragment(
                [icon = options.icon, active]() -> Nui::ElementRenderer {
                    if (icon.empty())
                        return Nui::nil();
                    return iconPanel({
                        .name = icon,
                        .color = active ? "var(--sapBrandColor)" : "#404040",
                        .withBorder = true
                    });
                }(),
                span{}([&options]() -> std::string {
                    if (options.sessionId.has_value())
                        return options.sessionId.value();

                    switch (options.thisSection) {
                        case Settings::Section::GeneralSettings:
                            return "General Settings";
                        case Settings::Section::GlobalInheritables:
                            return "Global Inheritables";
                        case Settings::Section::Session:
                            return "Unknown Session";
                        case Settings::Section::Add:
                            return "Add New!";
                    }
                }())
            );
        }
    );
    // clang-format on
}

Nui::ElementRenderer Settings::side()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.
    using Nui::Elements::span;

    // clang-format off
    return div{class_ = "side"}(
        div{class_ = "configuration-text"}(
            ui5::icon{
                "name"_prop = "settings",
                "design"_prop = "Neutral"
            }(),
            span{}("Configuration")
        ),
        sectionSelector({
            .thisSection = Settings::Section::GeneralSettings,
            .icon = "wrench",
        }),
        sectionSelector({
            .thisSection = Settings::Section::GlobalInheritables,
            .icon = "settings",
        }),
        div{style = "width: calc(100% - 20px); border-top: 1px solid gray; margin-bottom: 20px; margin-top: 10px"}(),
        div{class_ = "configuration-text"}(
            ui5::icon{
                "name"_prop = "it-system",
                "design"_prop = "Neutral"
            }(),
            span{}("Base Sessions / Servers")
        ),
        sectionSelector({
            .thisSection = Settings::Section::Add,
            .icon = "add",
        }),
        div{style = "display: flex; flex-direction: column;"}(
            impl_->sessionSelectors.map([this](long long, auto const& item) -> Nui::ElementRenderer {
                return sectionSelector({
                    .thisSection = Settings::Section::Session,
                    .sessionId = item.sessionId,
                    .icon = item.icon,
                });
            })
        )
    );
    // clang-format on
}