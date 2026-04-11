#include <frontend/icon_from_name.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <frontend/session_icon_options.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/text_input.hpp>
#include <script-nui-components/select.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <regex>

namespace Snc = ScriptNuiComponents;

struct NewSessionDialog::Implementation
{
    static constexpr std::string_view defaultName = "";

    std::string id;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    Nui::Observed<std::string> sessionName{std::string{defaultName}};
    Nui::Observed<ScriptNuiComponents::ValueState> nameValid{ScriptNuiComponents::ValueState::Valid};
    Nui::Observed<std::string> icon{"laptop"};
    Nui::Observed<std::string> sessionType{"ssh"};
    Nui::Observed<std::string> validationMessage{
        "Session name must be 1-5000 characters long and cannot contain \\ or / or \" characters."
    };
    Nui::Observed<bool> showIconPicker{true};
    Nui::Observed<bool> showSessionTypePicker{true};
    std::function<void(NewSessionDialog::ConfirmResult const&)> onConfirm;

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
    {}
};

NewSessionDialog::NewSessionDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, dialogBody());

    language->listenToLanguageChange(
        [this](std::string const&)
        {
            impl_->validationMessage = language->get("newSessionDialog", "validationMessage");
        }
    );
}

Nui::ElementRenderer NewSessionDialog::dialogBody()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    std::vector<std::string> iconOptions(std::begin(sessionIconOptions), std::end(sessionIconOptions));

    // clang-format off
    return section{class_ = "new-session-section"}(
        span{}(language->getObserved("newSessionDialog", "sessionNameLabel")),
        Snc::textInput({
            .value = impl_->sessionName,
            .attributes = {
                id = fmt::format("NewSessionDialogInput_{}", impl_->id),
                type = "Text",
                "keyup"_event = [this](Nui::WebApi::KeyboardEvent event) {
                    const auto target = event.target();
                    checkInputValue(target["value"].as<std::string>());
                }
            },
            .onChange = [this](std::string const& value, auto const&) {
                checkInputValue(value);
            },
            .valueState = &impl_->nameValid,
            .validationMessage = &impl_->validationMessage,
        }),
        div{
            style = observe(impl_->showSessionTypePicker).generate([](bool show) {
                return show ? "display: contents;" : "display: none;";
            })
        }(
            span{}(language->getObserved("newSessionDialog", "sessionTypeLabel")),
            Snc::select(Snc::SelectOptions<decltype(impl_->sessionType), std::vector<std::string>>{
                .activeOption = impl_->sessionType,
                .options = std::vector<std::string>{"ssh", "shell"},
                .activeRenderer = [](auto const& stateful) -> Nui::ElementRenderer
                {
                    return span{}(
                        observe(stateful.get()),
                        [](std::string const& typeStr) -> Nui::ElementRenderer {
                            return span{}(typeStr == "ssh" ? "SSH" : "Local Shell");
                        }
                    );
                },
                .elementRenderer = [](std::string const& typeStr) -> Nui::ElementRenderer
                {
                    return span{}(typeStr == "ssh" ? "SSH" : "Local Shell");
                },
                .makeId = [](){
                    return Nui::val::global("generateId")().as<std::string>();
                }
            })
        ),
        div{
            style = observe(impl_->showIconPicker).generate([](bool show) {
                return show ? "display: contents;" : "display: none;";
            })
        }(
            span{}(language->getObserved("newSessionDialog", "iconLabel")),
            Snc::select(Snc::SelectOptions<decltype(impl_->icon), std::vector<std::string>>{
                .activeOption = impl_->icon,
                .options = std::move(iconOptions),
                .popoverExtraClass = "new-session-icon-picker-popover",
                .activeRenderer = [](auto const& stateful) -> Nui::ElementRenderer
                {
                    return div{style = "display: flex; align-items: center; gap: 5px;"}(
                        observe(stateful.get()),
                        [](std::string const& iconName){
                            return fragment(iconFromName(iconName), span{style = "color: var(--color);"}(iconName));
                        }
                    );
                },
                .elementRenderer = [](std::string const& iconName) -> Nui::ElementRenderer
                {
                    return div{style = "display: flex; align-items: center; gap: 5px;"}(
                        fragment(iconFromName(iconName), span{style = "color: var(--color);"}(iconName))
                    );
                },
                .makeId = [](){
                    return Nui::val::global("generateId")().as<std::string>();
                }
            })
        )
    );
    // clang-format on
}

void NewSessionDialog::open(OpenOptions options)
{
    impl_->onConfirm = std::move(options.onConfirm);
    impl_->showIconPicker = options.showIconPicker;
    impl_->showSessionTypePicker = options.showSessionTypePicker;
    impl_->icon = options.initialIcon;
    impl_->sessionType = options.initialSessionType == SessionType::ssh ? "ssh" : "shell";
    if (options.initialName.empty())
    {
        impl_->sessionName = std::string{Implementation::defaultName};
        impl_->nameValid = ScriptNuiComponents::ValueState::Valid;
    }
    else
    {
        impl_->sessionName = options.initialName;
        checkInputValue(options.initialName);
    }
    Nui::globalEventContext.executeActiveEventsImmediately();

    impl_->dialog->open({
        .headerText = language->get("newSessionDialog", "title"),
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Cancel,
        .initialFocus = fmt::format("NewSessionDialogInput_{}", impl_->id),
        .onClose =
            [this](std::optional<Snc::Dialog::Button> button)
        {
            if (!impl_->onConfirm)
                return;

            if (button && *button == Snc::Dialog::Button::Ok &&
                impl_->nameValid.value() == ScriptNuiComponents::ValueState::Valid)
            {
                impl_->onConfirm(
                    ConfirmResult{
                        .sessionName = impl_->sessionName.value(),
                        .iconName = impl_->icon.value(),
                        .sessionType = impl_->sessionType.value() == "ssh" ? SessionType::ssh : SessionType::shell,
                    }
                );
            }
        },
        .modal = true,
        .mayCloseWithoutButton = false,
    });
}

void NewSessionDialog::checkInputValue(std::string const& value)
{
    std::regex validator(R"(^[^\\"\\\\/]{1,5000}$)");
    impl_->nameValid = std::regex_match(value, validator) ? ScriptNuiComponents::ValueState::Valid
                                                          : ScriptNuiComponents::ValueState::Invalid;
    Nui::WebApi::Console::log(
        fmt::format(
            "Checking session name validity: \"{}\" is {}",
            value,
            impl_->nameValid.value() == ScriptNuiComponents::ValueState::Valid ? "valid" : "invalid"
        )
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(NewSessionDialog);

Nui::ElementRenderer NewSessionDialog::operator()()
{
    return (*impl_->dialog)();
}