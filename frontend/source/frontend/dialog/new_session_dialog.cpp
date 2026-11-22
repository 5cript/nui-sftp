#include <frontend/icon_from_name.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/session_icon_options.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <frontend/settings/ftp_session_options.hpp>
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
#include <nui/frontend/api/keyboard_event.hpp>

#include <boost/describe/enum_from_string.hpp>
#include <boost/describe/enum_to_string.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <iterator>
#include <regex>

namespace Snc = ScriptNuiComponents;

namespace
{
    constexpr std::array<Persistence::TerminalEngineType, 3u> sessionTypes{
        Persistence::TerminalEngineType::ssh,
        Persistence::TerminalEngineType::ftp,
        Persistence::TerminalEngineType::shell,
    };

    constexpr std::array<Persistence::FtpTlsMode, 3u> tlsModes{
        Persistence::FtpTlsMode::Explicit,
        Persistence::FtpTlsMode::Implicit,
        Persistence::FtpTlsMode::Plain,
    };

    std::string sessionTypeName(Persistence::TerminalEngineType type)
    {
        return boost::describe::enum_to_string(type, "");
    }

    std::string sessionTypeLabel(std::string const& name)
    {
        if (name == "ssh")
            return "SSH";
        if (name == "ftp")
            return "FTP";
        return language->get("sessionFrontend", "localShell");
    }

    std::string tlsModeName(Persistence::FtpTlsMode tlsMode)
    {
        return boost::describe::enum_to_string(tlsMode, "");
    }

    std::optional<Persistence::FtpTlsMode> tlsModeNamed(std::string const& name)
    {
        Persistence::FtpTlsMode tlsMode{};
        if (boost::describe::enum_from_string(name.c_str(), tlsMode))
            return tlsMode;
        return std::nullopt;
    }

    std::string tlsModeLabel(std::string const& name)
    {
        const auto tlsMode = tlsModeNamed(name);
        if (!tlsMode)
            return name;
        return ftpTlsModeLabel(*tlsMode);
    }

    std::string trim(std::string const& text)
    {
        const auto notSpace = [](unsigned char character)
        {
            return !std::isspace(character);
        };
        const auto begin = std::find_if(text.begin(), text.end(), notSpace);
        const auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
        if (begin >= end)
            return {};
        return std::string{begin, end};
    }

    /**
     * @brief The port in the text, none for text that is no port from 1 to 65535.
     */
    std::optional<int> portOf(std::string const& text)
    {
        int port = 0;
        auto const* first = text.data();
        auto const* last = text.data() + text.size();
        const auto result = std::from_chars(first, last, port);
        if (result.ec != std::errc{} || result.ptr != last || port < 1 || port > 65535)
            return std::nullopt;
        return port;
    }

    void focusById(std::string const& fieldId)
    {
        auto element = Nui::val::global("document").call<Nui::val>("getElementById", fieldId);
        if (!element.isNull() && !element.isUndefined())
            element.call<void>("focus");
    }

    /**
     * @brief Lets the browser focus the field and show its validation message next to it.
     */
    void reportValidityById(std::string const& fieldId)
    {
        auto element = Nui::val::global("document").call<Nui::val>("getElementById", fieldId);
        if (!element.isNull() && !element.isUndefined())
            element.call<void>("reportValidity");
    }
}

struct NewSessionDialog::Implementation
{
    static constexpr std::string_view defaultName = "";

    std::string id;
    std::string nameInputId;
    std::string sessionTypeSelectId;
    std::string iconSelectId;
    std::string ftpHostInputId;
    std::string ftpPortInputId;
    std::string ftpUserInputId;
    std::string ftpTlsModeSelectId;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    Nui::Observed<std::string> sessionName{std::string{defaultName}};
    Nui::Observed<ScriptNuiComponents::ValueState> nameValid{ScriptNuiComponents::ValueState::Valid};
    Nui::Observed<std::string> icon{"laptop"};
    Nui::Observed<std::string> sessionType{"ssh"};
    Nui::Observed<std::string> validationMessage{language->get("newSessionDialog", "validationMessage")};

    Nui::Observed<std::string> ftpHost{""};
    Nui::Observed<std::string> ftpPort{""};
    Nui::Observed<std::string> ftpUser{""};
    Nui::Observed<std::string> ftpTlsModeName{tlsModeName(Persistence::FtpTlsMode::Explicit)};
    Nui::Observed<Persistence::FtpTlsMode> ftpTlsMode{Persistence::FtpTlsMode::Explicit};
    Nui::Observed<ScriptNuiComponents::ValueState> ftpHostValid{ScriptNuiComponents::ValueState::Invalid};
    Nui::Observed<ScriptNuiComponents::ValueState> ftpPortValid{ScriptNuiComponents::ValueState::Valid};
    Nui::Observed<std::string> ftpHostValidationMessage{language->get("newSessionDialog", "ftpHostValidationMessage")};
    Nui::Observed<std::string> ftpPortValidationMessage{language->get("newSessionDialog", "ftpPortValidationMessage")};

    LanguageChangeListener languageListener{};
    Nui::Observed<bool> showIconPicker{true};
    Nui::Observed<bool> showSessionTypePicker{true};
    bool confirmOnClose{false};
    std::function<void(NewSessionDialog::ConfirmResult const&)> onConfirm;

    Implementation(std::string id)
        : id{std::move(id)}
        , nameInputId{fmt::format("NewSessionDialogInput_{}", this->id)}
        , sessionTypeSelectId{fmt::format("NewSessionDialogSessionTypeSelect_{}", this->id)}
        , iconSelectId{fmt::format("NewSessionDialogIconSelect_{}", this->id)}
        , ftpHostInputId{fmt::format("NewSessionDialogFtpHost_{}", this->id)}
        , ftpPortInputId{fmt::format("NewSessionDialogFtpPort_{}", this->id)}
        , ftpUserInputId{fmt::format("NewSessionDialogFtpUser_{}", this->id)}
        , ftpTlsModeSelectId{fmt::format("NewSessionDialogFtpTlsMode_{}", this->id)}
        , dialog{}
    {}

    bool showsFtpServer() const
    {
        return showSessionTypePicker.value() &&
            sessionType.value() == sessionTypeName(Persistence::TerminalEngineType::ftp);
    }
};

NewSessionDialog::NewSessionDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, dialogBody());

    impl_->languageListener = language->listenToLanguageChange(
        [this](std::string const&)
        {
            impl_->validationMessage = language->get("newSessionDialog", "validationMessage");
            impl_->ftpHostValidationMessage = language->get("newSessionDialog", "ftpHostValidationMessage");
            impl_->ftpPortValidationMessage = language->get("newSessionDialog", "ftpPortValidationMessage");
        }
    );
}

std::vector<std::string> NewSessionDialog::visibleFieldIds() const
{
    std::vector<std::string> fields;
    fields.push_back(impl_->nameInputId);
    if (impl_->showSessionTypePicker.value())
        fields.push_back(impl_->sessionTypeSelectId);
    if (impl_->showsFtpServer())
    {
        fields.push_back(impl_->ftpHostInputId);
        fields.push_back(impl_->ftpPortInputId);
        fields.push_back(impl_->ftpUserInputId);
        fields.push_back(impl_->ftpTlsModeSelectId);
    }
    if (impl_->showIconPicker.value())
        fields.push_back(impl_->iconSelectId);
    return fields;
}

bool NewSessionDialog::focusNeighbour(std::string const& currentId, int direction) const
{
    const auto fields = visibleFieldIds();
    const auto current = std::find(fields.begin(), fields.end(), currentId);
    if (current == fields.end())
        return false;
    const auto target = std::distance(fields.begin(), current) + direction;
    if (target < 0 || target >= std::ssize(fields))
        return false;
    focusById(fields[static_cast<std::size_t>(target)]);
    return true;
}

void NewSessionDialog::confirmIfValid()
{
    if (!inputIsValid())
        return reportInvalidInput();
    impl_->confirmOnClose = true;
    impl_->dialog->close();
}

std::function<void(Nui::val)> NewSessionDialog::selectKeyHandler(std::string fieldId)
{
    return [this, fieldId = std::move(fieldId)](Nui::val eventValue)
    {
        Nui::WebApi::KeyboardEvent event{eventValue};
        if (event.key() != "Enter")
            return;
        event.preventDefault();
        if (!focusNeighbour(fieldId, +1))
            confirmIfValid();
    };
}

std::function<void(Nui::val)>
NewSessionDialog::textKeyHandler(std::string fieldId, std::function<bool(std::string const&)> syncValue)
{
    return [this, fieldId = std::move(fieldId), syncValue = std::move(syncValue)](Nui::val eventValue)
    {
        Nui::WebApi::KeyboardEvent event{eventValue};
        const auto key = event.key();
        if (key != "Enter" && key != "ArrowDown")
            return;
        event.preventDefault();
        const bool valid = syncValue(event.target()["value"].as<std::string>());
        if (key == "ArrowDown")
        {
            focusNeighbour(fieldId, +1);
            return;
        }
        if (!valid)
        {
            // The message reaches the input with the events of its value state.
            Nui::globalEventContext.executeActiveEventsImmediately();
            return reportValidityById(fieldId);
        }
        if (!focusNeighbour(fieldId, +1))
            confirmIfValid();
    };
}

Nui::ElementRenderer NewSessionDialog::ftpServerFields()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    std::vector<std::string> tlsModeNames;
    for (const auto tlsMode : tlsModes)
        tlsModeNames.push_back(tlsModeName(tlsMode));

    // clang-format off
    return div{
        class_ = "new-session-ftp-server",
        style = observe(impl_->sessionType, impl_->showSessionTypePicker).generate([this]() {
            return impl_->showsFtpServer() ? "display: contents;" : "display: none;";
        })
    }(
        span{}(language->getObserved("newSessionDialog", "ftpHostLabel")),
        Snc::textInput({
            .value = impl_->ftpHost,
            .attributes = {
                id = impl_->ftpHostInputId,
                type = "Text",
                "keyup"_event = [this](Nui::WebApi::KeyboardEvent event) {
                    checkFtpHostValue(event.target()["value"].as<std::string>());
                },
                "keydown"_event = textKeyHandler(impl_->ftpHostInputId, [this](std::string const& value) {
                    impl_->ftpHost = value;
                    checkFtpHostValue(value);
                    return impl_->ftpHostValid.value() == Snc::ValueState::Valid;
                }),
            },
            .onChange = [this](std::string const& value, auto const&) {
                checkFtpHostValue(value);
            },
            .valueState = &impl_->ftpHostValid,
            .validationMessage = &impl_->ftpHostValidationMessage,
        }),
        span{}(language->getObserved("newSessionDialog", "ftpPortLabel")),
        Snc::textInput({
            .value = impl_->ftpPort,
            .attributes = {
                id = impl_->ftpPortInputId,
                type = "Number",
                "placeholder"_attr = observe(impl_->ftpTlsMode).generate([](Persistence::FtpTlsMode tlsMode) {
                    return std::to_string(Persistence::defaultFtpPort(tlsMode));
                }),
                "keyup"_event = [this](Nui::WebApi::KeyboardEvent event) {
                    checkFtpPortValue(event.target()["value"].as<std::string>());
                },
                "keydown"_event = textKeyHandler(impl_->ftpPortInputId, [this](std::string const& value) {
                    impl_->ftpPort = value;
                    checkFtpPortValue(value);
                    return impl_->ftpPortValid.value() == Snc::ValueState::Valid;
                }),
            },
            .onChange = [this](std::string const& value, auto const&) {
                checkFtpPortValue(value);
            },
            .valueState = &impl_->ftpPortValid,
            .validationMessage = &impl_->ftpPortValidationMessage,
        }),
        span{}(language->getObserved("newSessionDialog", "ftpUserLabel")),
        Snc::textInput({
            .value = impl_->ftpUser,
            .attributes = {
                id = impl_->ftpUserInputId,
                type = "Text",
                "placeholder"_attr = language->getObserved("newSessionDialog", "ftpUserPlaceholder"),
                "keydown"_event = textKeyHandler(impl_->ftpUserInputId, [this](std::string const& value) {
                    impl_->ftpUser = value;
                    return true;
                }),
            },
        }),
        span{}(language->getObserved("newSessionDialog", "ftpTlsModeLabel")),
        Snc::select(Snc::SelectOptions<decltype(impl_->ftpTlsModeName), std::vector<std::string>>{
            .activeOption = impl_->ftpTlsModeName,
            .options = std::move(tlsModeNames),
            .attributes = {
                id = impl_->ftpTlsModeSelectId,
                "keydown"_event = selectKeyHandler(impl_->ftpTlsModeSelectId),
            },
            .onChange = [this](std::string const& name, Nui::WebApi::MouseEvent const&) {
                const auto tlsMode = tlsModeNamed(name);
                if (!tlsMode)
                {
                    Log::error("New session dialog: unknown TLS mode '{}'.", name);
                    return;
                }
                impl_->ftpTlsMode = *tlsMode;
            },
            .activeRenderer = [](auto const& stateful) -> Nui::ElementRenderer
            {
                return span{}(
                    observe(stateful.get()),
                    [](std::string const& name) -> Nui::ElementRenderer {
                        return span{}(tlsModeLabel(name));
                    }
                );
            },
            .elementRenderer = [](std::string const& name) -> Nui::ElementRenderer
            {
                return span{}(tlsModeLabel(name));
            },
            .makeId = [](){
                return Nui::val::global("generateId")().as<std::string>();
            }
        })
    );
    // clang-format on
}

Nui::ElementRenderer NewSessionDialog::dialogBody()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    std::vector<std::string> iconOptions(std::begin(sessionIconOptions), std::end(sessionIconOptions));
    std::vector<std::string> sessionTypeNames;
    for (const auto type : sessionTypes)
        sessionTypeNames.push_back(sessionTypeName(type));

    // clang-format off
    return section{class_ = "new-session-section"}(
        span{}(language->getObserved("newSessionDialog", "sessionNameLabel")),
        Snc::textInput({
            .value = impl_->sessionName,
            .attributes = {
                id = impl_->nameInputId,
                type = "Text",
                "keyup"_event = [this](Nui::WebApi::KeyboardEvent event) {
                    const auto target = event.target();
                    checkInputValue(target["value"].as<std::string>());
                },
                "keydown"_event = textKeyHandler(impl_->nameInputId, [this](std::string const& value) {
                    impl_->sessionName = value;
                    checkInputValue(value);
                    return impl_->nameValid.value() == Snc::ValueState::Valid;
                }),
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
                .options = std::move(sessionTypeNames),
                .attributes = {
                    id = impl_->sessionTypeSelectId,
                    "keydown"_event = selectKeyHandler(impl_->sessionTypeSelectId),
                },
                .activeRenderer = [](auto const& stateful) -> Nui::ElementRenderer
                {
                    return span{}(
                        observe(stateful.get()),
                        [](std::string const& typeName) -> Nui::ElementRenderer {
                            return span{}(sessionTypeLabel(typeName));
                        }
                    );
                },
                .elementRenderer = [](std::string const& typeName) -> Nui::ElementRenderer
                {
                    return span{}(sessionTypeLabel(typeName));
                },
                .makeId = [](){
                    return Nui::val::global("generateId")().as<std::string>();
                }
            })
        ),
        ftpServerFields(),
        div{
            style = observe(impl_->showIconPicker).generate([](bool show) {
                return show ? "display: contents;" : "display: none;";
            })
        }(
            span{}(language->getObserved("newSessionDialog", "iconLabel")),
            Snc::select(Snc::SelectOptions<decltype(impl_->icon), std::vector<std::string>>{
                .activeOption = impl_->icon,
                .options = std::move(iconOptions),
                .attributes = {
                    id = impl_->iconSelectId,
                    "keydown"_event = selectKeyHandler(impl_->iconSelectId),
                },
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
    impl_->sessionType = sessionTypeName(options.initialSessionType);
    impl_->confirmOnClose = false;
    impl_->sessionName = options.initialName.empty() ? std::string{Implementation::defaultName} : options.initialName;
    impl_->ftpHost = "";
    impl_->ftpPort = "";
    impl_->ftpUser = "";
    impl_->ftpTlsModeName = tlsModeName(Persistence::FtpTlsMode::Explicit);
    impl_->ftpTlsMode = Persistence::FtpTlsMode::Explicit;
    checkInputValue(impl_->sessionName.value());
    checkFtpHostValue(impl_->ftpHost.value());
    checkFtpPortValue(impl_->ftpPort.value());
    Nui::globalEventContext.executeActiveEventsImmediately();

    impl_->dialog->setButtonLabels(localizedButtonLabels());
    impl_->dialog->open({
        .headerText = language->get("newSessionDialog", "title"),
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Cancel,
        .initialFocus = impl_->nameInputId,
        .onClose =
            [this](std::optional<Snc::Dialog::Button> button)
        {
            if (!impl_->onConfirm)
                return;

            const bool okPressed = button && *button == Snc::Dialog::Button::Ok;
            const bool confirmed = okPressed || impl_->confirmOnClose;
            impl_->confirmOnClose = false;

            if (!confirmed || !inputIsValid())
                return;

            const auto sessionType = Persistence::terminalEngineTypeFromName(impl_->sessionType.value());
            if (!sessionType)
            {
                Log::error("New session dialog: unknown session type '{}'.", impl_->sessionType.value());
                return;
            }
            impl_->onConfirm(
                ConfirmResult{
                    .sessionName = impl_->sessionName.value(),
                    .iconName = impl_->icon.value(),
                    .sessionType = *sessionType,
                    .ftpServer = ftpServer(),
                }
            );
        },
        .onButton =
            [this](Snc::Dialog::Button button)
        {
            if (button != Snc::Dialog::Button::Ok || inputIsValid())
                return true;
            reportInvalidInput();
            return false;
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

void NewSessionDialog::checkFtpHostValue(std::string const& value)
{
    impl_->ftpHostValid = trim(value).empty() ? Snc::ValueState::Invalid : Snc::ValueState::Valid;
}

void NewSessionDialog::checkFtpPortValue(std::string const& value)
{
    const auto trimmed = trim(value);
    impl_->ftpPortValid = trimmed.empty() || portOf(trimmed) ? Snc::ValueState::Valid : Snc::ValueState::Invalid;
}

std::optional<std::string> NewSessionDialog::firstInvalidFieldId() const
{
    if (impl_->nameValid.value() != Snc::ValueState::Valid)
        return impl_->nameInputId;
    if (!impl_->showsFtpServer())
        return std::nullopt;
    if (impl_->ftpHostValid.value() != Snc::ValueState::Valid)
        return impl_->ftpHostInputId;
    if (impl_->ftpPortValid.value() != Snc::ValueState::Valid)
        return impl_->ftpPortInputId;
    return std::nullopt;
}

bool NewSessionDialog::inputIsValid() const
{
    return !firstInvalidFieldId();
}

void NewSessionDialog::reportInvalidInput() const
{
    const auto invalid = firstInvalidFieldId();
    if (!invalid)
        return;
    // The messages reach the inputs with the events of their value states.
    Nui::globalEventContext.executeActiveEventsImmediately();
    reportValidityById(*invalid);
}

NewSessionDialog::FtpServer NewSessionDialog::ftpServer() const
{
    return FtpServer{
        .host = trim(impl_->ftpHost.value()),
        .port = portOf(trim(impl_->ftpPort.value())),
        .userName = trim(impl_->ftpUser.value()),
        .tlsMode = impl_->ftpTlsMode.value(),
    };
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(NewSessionDialog);

Nui::ElementRenderer NewSessionDialog::operator()()
{
    return (*impl_->dialog)();
}