#include <frontend/settings/layout_setting.hpp>

#include <ui5/components/select.hpp>
#include <ui5/components/label.hpp>
#include <ui5/components/button.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

LayoutSetting::LayoutSetting(
    LanguageObservedText helpText,
    std::function<void()> onChange,
    std::function<std::optional<nlohmann::json>()> obtainCurrentLayout,
    ConfirmDialog& confirmDialog,
    InputDialog& newItemDialog
)
    : SettingBase{std::move(helpText), std::move(onChange), []() {}}
    , confirmDialog_{&confirmDialog}
    , newItemDialog_{&newItemDialog}
    , selected_{}
    , obtainCurrentLayout_{std::move(obtainCurrentLayout)}
{}

Nui::ElementRenderer LayoutSetting::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{class_ = "layout-setting-container"}
    (
        ui5::label{
            style = "color: var(--sapTextColor); margin-right: 10px", "showColon"_prop = true
        }(language->getObserved("settings", "layoutSetting", "layoutKeysLabel")),
        div{class_ = "layout-setting-controls"}(
            ui5::select{
                "value"_prop = selected_,
                "disabled"_prop = observe(state_).generate(
                    [](auto const& state)
                    {
                        return state.empty();
                    }
                ),
                "change"_event =
                    [this](Nui::val event)
                {
                    selected_ = event["detail"]["selectedOption"]["key"].as<std::string>();
                },
            }(
                Nui::range(state_),
                [](long long, std::pair<std::string, nlohmann::json> const& element)
                {
                    return ui5::option{
                        "key"_prop = element.first
                    }(element.first);
                }
            ),
            ui5::button{
                "design"_prop = "Transparent",
                "icon"_prop = "add",
                "click"_event =
                    [this]()
                {
                    newItemDialog_->open({
                        .whatFor = language->get("settings", "layoutSetting", "addLayoutKeyBody"),
                        .headerText = language->get("settings", "layoutSetting", "addLayoutKeyHeader"),
                        .onConfirm = [this](std::optional<std::string> const& newKey)
                        {
                            if (!newKey || newKey->empty())
                                return;

                            auto& currentKeys = *state_;
                            if (currentKeys.contains(*newKey))
                            {
                                // Key already exists:
                                confirmDialog_->open({
                                    .state = ConfirmDialog::State::Negative,
                                    .headerText = language->get("settings", "layoutSetting", "addLayoutKeyErrorHeader"),
                                    .text = language->get("settings", "layoutSetting", "addLayoutKeyErrorText"),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                                return;
                            }

                            if (auto currentLayout = obtainCurrentLayout_())
                            {
                                currentKeys[*newKey] = *currentLayout;
                                selected_ = *newKey;
                                state_.modifyNow();
                                onChange_();
                            }
                            else
                            {
                                confirmDialog_->open({
                                    .state = ConfirmDialog::State::Negative,
                                    .headerText = language->get("settings", "layoutSetting", "saveLayoutErrorHeader"),
                                    .text = language->get("settings", "layoutSetting", "saveLayoutErrorText"),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                            }
                        },
                    });
                },
            }(language->getObserved("settings", "layoutSetting", "saveCurrentLayout")),
            ui5::button{
                "design"_prop = "Transparent",
                "icon"_prop = "delete",
                "click"_event =
                    [this]()
                {
                    if (state_->empty())
                        return;

                    const auto keyToRemove = *selected_;

                    confirmDialog_->open({
                        .state = ConfirmDialog::State::Critical,
                        .headerText = language->get("settings", "layoutSetting", "removeLayoutKeyConfirmHeader"),
                        .text = fmt::format(
                            fmt::runtime(
                                language->get("settings", "layoutSetting", "removeLayoutKeyConfirmText") + ": {}"
                            ),
                            keyToRemove
                        ),
                        .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
                        .onClose = [this, keyToRemove](ConfirmDialog::Button button)
                        {
                            if (button != ConfirmDialog::Button::Ok)
                                return;

                            state_.value().erase(keyToRemove);
                            state_.modifyNow();
                            onChange_();
                        },
                    });
                },
            }()
        ),
        SettingBase::reset(),
        SettingBase::help()
    );
    // clang-format on
}