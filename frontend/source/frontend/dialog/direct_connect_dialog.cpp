#include <frontend/dialog/direct_connect_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/select.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/filesystem/file_dialog.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>

namespace
{
    /**
     * @brief One kind of inheritable options a direct connect can be based on.
     */
    struct InheritableKind
    {
        /**
         * @brief The language key of the group name, below "settings".
         */
        char const* groupNameKey;
        std::string Persistence::InheritableReferences::* reference;
        std::vector<std::string> (*groupNames)(Persistence::State const& state);
    };

    template <typename MapT>
    std::vector<std::string> keysOf(MapT const& map)
    {
        std::vector<std::string> keys;
        keys.reserve(map.size());
        for (auto const& [key, value] : map)
            keys.push_back(key);
        return keys;
    }

    constexpr std::size_t inheritableKindCount = 6u;

    const std::array<InheritableKind, inheritableKindCount> inheritableKinds{{
        {"sshOptionsGroupName",
            &Persistence::InheritableReferences::sshOptions,
            [](Persistence::State const& state)
            {
                return keysOf(state.sshOptions);
            }},
        {"sftpOptionsGroupName",
            &Persistence::InheritableReferences::sftpOptions,
            [](Persistence::State const& state)
            {
                return keysOf(state.sftpOptions);
            }},
        {"terminalOptionsGroupName",
            &Persistence::InheritableReferences::terminalOptions,
            [](Persistence::State const& state)
            {
                return keysOf(state.terminalOptions);
            }},
        {"termiosGroupName",
            &Persistence::InheritableReferences::termios,
            [](Persistence::State const& state)
            {
                return keysOf(state.termios);
            }},
        {"queueOptionsGroupName",
            &Persistence::InheritableReferences::queueOptions,
            [](Persistence::State const& state)
            {
                return keysOf(state.queueOptions);
            }},
        {"historyOptionsGroupName",
            &Persistence::InheritableReferences::historyOptions,
            [](Persistence::State const& state)
            {
                return keysOf(state.historyOptions);
            }},
    }};

    std::string trim(std::string const& str)
    {
        const auto notSpace = [](unsigned char ch) {
            return !std::isspace(ch);
        };
        auto begin = std::find_if(str.begin(), str.end(), notSpace);
        auto end = std::find_if(str.rbegin(), str.rend(), notSpace).base();
        if (begin >= end)
            return {};
        return std::string{begin, end};
    }
}

namespace Snc = ScriptNuiComponents;

struct DirectConnectDialog::Implementation
{
    std::string id;
    Persistence::StateHolder* stateHolder;
    std::unique_ptr<Snc::Dialog> dialog;

    Nui::Observed<std::string> host{""};
    Nui::Observed<std::string> port{""};
    Nui::Observed<std::string> user{""};
    Nui::Observed<std::string> sshKeyPrivate{""};

    /**
     * @brief The picked group of each inheritable kind, in the order of inheritableKinds.
     */
    std::array<Nui::Observed<std::string>, inheritableKindCount> inheritableChoices{};
    std::array<Nui::Observed<std::vector<std::string>>, inheritableKindCount> inheritableOptions{};

    Nui::Observed<Snc::ValueState> hostValid{Snc::ValueState::Invalid};
    Nui::Observed<Snc::ValueState> portValid{Snc::ValueState::Valid};

    Nui::Observed<std::string> hostValidationMessage{""};
    Nui::Observed<std::string> portValidationMessage{""};

    bool loadedFromState{false};
    LanguageChangeListener languageListener{};
    bool confirmOnClose{false};
    std::function<void(DirectConnectDialog::ConfirmResult const&)> onConfirm;

    Implementation(std::string ident, Persistence::StateHolder* holder)
        : id{std::move(ident)}
        , stateHolder{holder}
        , dialog{}
    {}
};

DirectConnectDialog::DirectConnectDialog(std::string id, Persistence::StateHolder* stateHolder)
    : impl_{std::make_unique<Implementation>(std::move(id), stateHolder)}
{
    impl_->dialog = std::make_unique<Snc::Dialog>(impl_->id, dialogBody());

    impl_->hostValidationMessage = language->get("directConnectDialog", "hostValidationMessage");
    impl_->portValidationMessage = language->get("directConnectDialog", "portValidationMessage");
    impl_->languageListener = language->listenToLanguageChange(
        [this](std::string const&)
        {
            impl_->hostValidationMessage = language->get("directConnectDialog", "hostValidationMessage");
            impl_->portValidationMessage = language->get("directConnectDialog", "portValidationMessage");
        }
    );
}

void DirectConnectDialog::loadFromState()
{
    if (impl_->loadedFromState)
        return;
    impl_->loadedFromState = true;

    auto const& state = impl_->stateHolder->stateCache();

    const auto inheritables = state.lastDirectConnectInheritables.value_or(Persistence::InheritableReferences{});
    for (std::size_t index = 0; index < inheritableKindCount; ++index)
        impl_->inheritableChoices[index] = inheritables.*(inheritableKinds[index].reference);

    if (!state.lastDirectConnect)
        return;

    auto const& saved = *state.lastDirectConnect;
    const auto user = saved.loginUser();
    impl_->host = saved.host;
    impl_->port = saved.port ? std::to_string(*saved.port) : std::string{};
    impl_->user = user.name.value_or("");
    impl_->sshKeyPrivate = user.sshKeyPrivate ? user.sshKeyPrivate->string() : std::string{};
}

void DirectConnectDialog::refreshInheritableOptions()
{
    auto const& state = impl_->stateHolder->stateCache();
    for (std::size_t index = 0; index < inheritableKindCount; ++index)
    {
        auto groupNames = inheritableKinds[index].groupNames(state);
        auto& choice = impl_->inheritableChoices[index];
        if (std::find(groupNames.begin(), groupNames.end(), choice.value()) == groupNames.end())
        {
            const auto hasDefault = std::find(groupNames.begin(), groupNames.end(), "default") != groupNames.end();
            choice = hasDefault || groupNames.empty() ? std::string{"default"} : groupNames.front();
        }
        impl_->inheritableOptions[index] = std::move(groupNames);
    }
}

Nui::ElementRenderer DirectConnectDialog::inheritablesGrid()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    std::vector<Nui::ElementRenderer> cells;
    cells.reserve(inheritableKindCount * 2u);
    for (std::size_t index = 0; index < inheritableKindCount; ++index)
    {
        auto& choice = impl_->inheritableChoices[index];
        auto& options = impl_->inheritableOptions[index];
        cells.push_back(span{}(language->getObserved("settings", inheritableKinds[index].groupNameKey)));
        cells.push_back(
            Snc::select(
                Snc::SelectOptions<decltype(choice), decltype(options)>{
                    .activeOption = choice,
                    .options = options,
                    .attributes =
                        {
                            disabled = Nui::observe(options).generate(
                                [](std::vector<std::string> const& names) -> std::optional<bool>
                                {
                                    return names.empty() ? std::optional<bool>{true} : std::nullopt;
                                }
                            ),
                        },
                    .makeId = []()
                    {
                        return Nui::val::global("generateId")().as<std::string>();
                    },
                }
            )
        );
    }

    // clang-format off
    return div{class_ = "direct-connect-inheritables"}(
        div{class_ = "direct-connect-separator"}(
            span{}(language->getObserved("directConnectDialog", "inheritablesHeader"))
        ),
        div{class_ = "direct-connect-inheritables-grid"}(
            Nui::range(std::move(cells)),
            [](long long, auto const& cell) -> Nui::ElementRenderer {
                return cell;
            }
        )
    );
    // clang-format on
}

Nui::ElementRenderer DirectConnectDialog::dialogBody()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    const auto hostInputId = fmt::format("DirectConnectDialogHost_{}", impl_->id);
    const auto portInputId = fmt::format("DirectConnectDialogPort_{}", impl_->id);
    const auto userInputId = fmt::format("DirectConnectDialogUser_{}", impl_->id);
    const auto keyInputId = fmt::format("DirectConnectDialogKey_{}", impl_->id);

    const std::array<std::string, 4> fieldOrder{hostInputId, portInputId, userInputId, keyInputId};

    auto focusById = [](std::string const& fieldId) {
        auto doc = Nui::val::global("document");
        auto elem = doc.call<Nui::val>("getElementById", fieldId);
        if (!elem.isNull() && !elem.isUndefined())
            elem.call<void>("focus");
    };

    auto navigate = [fieldOrder, focusById](std::string const& currentId, int direction) -> bool {
        for (std::size_t idx = 0; idx < fieldOrder.size(); ++idx)
        {
            if (fieldOrder[idx] != currentId)
                continue;
            const long long target = static_cast<long long>(idx) + direction;
            if (target < 0 || target >= static_cast<long long>(fieldOrder.size()))
                return false;
            focusById(fieldOrder[static_cast<std::size_t>(target)]);
            return true;
        }
        return false;
    };

    auto confirmIfValid = [this]() {
        if (impl_->hostValid.value() != Snc::ValueState::Valid)
            return;
        if (impl_->portValid.value() != Snc::ValueState::Valid)
            return;
        impl_->confirmOnClose = true;
        impl_->dialog->close();
    };

    auto enterHandler = [navigate, confirmIfValid](
                            std::string fieldId,
                            std::function<void(std::string const&)> syncValue
                        ) {
        return [navigate, confirmIfValid, fieldId, syncValue = std::move(syncValue)](
                   Nui::WebApi::KeyboardEvent event
               ) {
            if (event.key() != "Enter")
                return;
            event.preventDefault();
            const auto currentValue = event.target()["value"].as<std::string>();
            if (syncValue)
                syncValue(currentValue);
            if (!navigate(fieldId, +1))
                confirmIfValid();
        };
    };

    auto makeCell = [](Nui::ElementRenderer labelRenderer, Nui::ElementRenderer inputRenderer) {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        return div{class_ = "direct-connect-cell"}(
            std::move(labelRenderer),
            std::move(inputRenderer)
        );
    };

    // clang-format off
    return section{class_ = "direct-connect-section"}(
        makeCell(
            span{}(language->getObserved("directConnectDialog", "hostLabel")),
            Snc::textInput({
                .value = impl_->host,
                .attributes = {
                    id = hostInputId,
                    type = "Text",
                    "keyup"_event = [this](Nui::WebApi::KeyboardEvent event) {
                        const auto target = event.target();
                        checkHostValue(target["value"].as<std::string>());
                    },
                    "keydown"_event = enterHandler(
                        hostInputId,
                        [this](std::string const& value) {
                            impl_->host = value;
                            checkHostValue(value);
                        }
                    ),
                },
                .onChange = [this](std::string const& value, auto const&) {
                    checkHostValue(value);
                },
                .valueState = &impl_->hostValid,
                .validationMessage = &impl_->hostValidationMessage,
            })
        ),
        makeCell(
            span{}(language->getObserved("directConnectDialog", "portLabel")),
            Snc::textInput({
                .value = impl_->port,
                .attributes = {
                    id = portInputId,
                    type = "Number",
                    "keyup"_event = [this](Nui::WebApi::KeyboardEvent event) {
                        const auto target = event.target();
                        checkPortValue(target["value"].as<std::string>());
                    },
                    "keydown"_event = enterHandler(
                        portInputId,
                        [this](std::string const& value) {
                            impl_->port = value;
                            checkPortValue(value);
                        }
                    ),
                },
                .onChange = [this](std::string const& value, auto const&) {
                    checkPortValue(value);
                },
                .valueState = &impl_->portValid,
                .validationMessage = &impl_->portValidationMessage,
            })
        ),
        makeCell(
            span{}(language->getObserved("directConnectDialog", "userLabel")),
            Snc::textInput({
                .value = impl_->user,
                .attributes = {
                    id = userInputId,
                    type = "Text",
                    "keydown"_event = enterHandler(
                        userInputId,
                        [this](std::string const& value) { impl_->user = value; }
                    ),
                },
            })
        ),
        makeCell(
            span{}(language->getObserved("directConnectDialog", "sshKeyPrivateLabel")),
            div{class_ = "direct-connect-path-row"}(
                Snc::textInput({
                    .value = impl_->sshKeyPrivate,
                    .attributes = {
                        id = keyInputId,
                        type = "Text",
                        "keydown"_event = enterHandler(
                            keyInputId,
                            [this](std::string const& value) { impl_->sshKeyPrivate = value; }
                        ),
                    },
                }),
                Snc::button({
                    .text = language->get("settings", "pathSetting", "browseButton"),
                    .attributes = {
                        onClick = [this](auto const&) {
                            Nui::FileDialog::showOpenDialog(
                                {
                                    .title = language->get("directConnectDialog", "pickPrivateKey"),
                                    .defaultPath = "%userprofile%",
                                    .filters = {},
                                    .forcePath = false,
                                    .allowMultiSelect = false,
                                },
                                [this](std::optional<std::vector<std::filesystem::path>> results) {
                                    if (!results || results->empty())
                                        return;
                                    impl_->sshKeyPrivate = results->at(0).string();
                                    Nui::globalEventContext.executeActiveEventsImmediately();
                                }
                            );
                        },
                    },
                    .styleVariant = Snc::StyleVariant::Regular,
                })
            )
        ),
        inheritablesGrid()
    );
    // clang-format on
}

void DirectConnectDialog::checkHostValue(std::string const& value)
{
    impl_->hostValid = trim(value).empty() ? Snc::ValueState::Invalid : Snc::ValueState::Valid;
}

void DirectConnectDialog::checkPortValue(std::string const& value)
{
    const auto trimmed = trim(value);
    if (trimmed.empty())
    {
        impl_->portValid = Snc::ValueState::Valid;
        return;
    }
    int parsed = 0;
    auto const* first = trimmed.data();
    auto const* last = trimmed.data() + trimmed.size();
    auto const result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed < 1 || parsed > 65535)
    {
        impl_->portValid = Snc::ValueState::Invalid;
        return;
    }
    impl_->portValid = Snc::ValueState::Valid;
}

void DirectConnectDialog::open(OpenOptions options)
{
    loadFromState();
    refreshInheritableOptions();

    impl_->onConfirm = std::move(options.onConfirm);
    impl_->confirmOnClose = false;

    checkHostValue(impl_->host.value());
    checkPortValue(impl_->port.value());
    Nui::globalEventContext.executeActiveEventsImmediately();

    impl_->dialog->setButtonLabels(localizedButtonLabels());
    impl_->dialog->open({
        .headerText = language->get("directConnectDialog", "title"),
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Cancel,
        .initialFocus = fmt::format("DirectConnectDialogHost_{}", impl_->id),
        .onClose =
            [this](std::optional<Snc::Dialog::Button> button)
        {
            const bool okPressed = button && *button == Snc::Dialog::Button::Ok;
            const bool confirmed = okPressed || impl_->confirmOnClose;
            impl_->confirmOnClose = false;

            if (!confirmed)
                return;
            if (impl_->hostValid.value() != Snc::ValueState::Valid)
                return;
            if (impl_->portValid.value() != Snc::ValueState::Valid)
                return;

            const auto hostTrimmed = trim(impl_->host.value());
            const auto portTrimmed = trim(impl_->port.value());
            const auto userTrimmed = trim(impl_->user.value());
            const auto keyTrimmed = trim(impl_->sshKeyPrivate.value());

            Persistence::SshSessionOptions sshOpts{};
            sshOpts.host = hostTrimmed;

            if (!portTrimmed.empty())
            {
                int parsed = 0;
                auto const* first = portTrimmed.data();
                auto const* last = portTrimmed.data() + portTrimmed.size();
                auto const result = std::from_chars(first, last, parsed);
                if (result.ec == std::errc{} && result.ptr == last)
                    sshOpts.port = parsed;
            }

            if (!userTrimmed.empty() || !keyTrimmed.empty())
            {
                Persistence::SshUser user{};
                if (!userTrimmed.empty())
                    user.name = userTrimmed;
                if (!keyTrimmed.empty())
                    user.sshKeyPrivate = std::filesystem::path{keyTrimmed};
                sshOpts.users.push_back(std::move(user));
            }

            Persistence::InheritableReferences inheritables{};
            for (std::size_t index = 0; index < inheritableKindCount; ++index)
                inheritables.*(inheritableKinds[index].reference) = impl_->inheritableChoices[index].value();

            impl_->stateHolder->loadModifySave(
                [sshOpts, inheritables](Persistence::State& state)
                {
                    state.lastDirectConnect = sshOpts;
                    state.lastDirectConnectInheritables = inheritables;
                }
            );

            if (impl_->onConfirm)
                impl_->onConfirm(
                    ConfirmResult{.sshOptions = std::move(sshOpts), .inheritables = std::move(inheritables)}
                );
        },
        .modal = true,
        .mayCloseWithoutButton = false,
    });
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(DirectConnectDialog);

Nui::ElementRenderer DirectConnectDialog::operator()()
{
    return (*impl_->dialog)();
}
