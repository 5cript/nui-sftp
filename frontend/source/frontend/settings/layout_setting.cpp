#include <frontend/settings/layout_setting.hpp>

#include <script-nui-components/select.hpp>
#include <script-nui-components/button.hpp>

#include <frontend/svgs/add.hpp>
#include <frontend/svgs/delete.hpp>

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
    namespace Snc = ScriptNuiComponents;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{class_ = "layout-setting-container"}
    (
        span{
            style = "color: var(--sapTextColor); margin-right: 10px"
        }(language->getObserved("settings", "layoutSetting", "layoutKeysLabel")),
        div{class_ = "layout-setting-controls"}(
            Snc::select(Snc::SelectOptions<decltype(selected_), decltype(state_)>{
                .activeOption = selected_,
                .options = state_,
                .attributes {
                    disabled = observe(state_).generate([](auto const& state) {
                        return state.empty();
                    }),
                },
                .onChange = [this](auto const&, auto const& /*event*/)
                {
                    onChange_();
                },
            }),
            Snc::button({
                .text = language->get("settings", "layoutSetting", "saveCurrentLayout"),
                .icon = GeneratedSvgs::add(),
                .attributes = {
                    onClick = [this]() {
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
                                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
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
                                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                    .headerText = language->get("settings", "layoutSetting", "saveLayoutErrorHeader"),
                                    .text = language->get("settings", "layoutSetting", "saveLayoutErrorText"),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                            }
                        },
                    });
                    }
                },
                .styleVariant = ScriptNuiComponents::StyleVariant::Transparent,
            }),
            Snc::button({
                .text = language->get("settings", "layoutSetting", "deleteLayoutKey"),
                .icon = GeneratedSvgs::delete_(),
                .attributes = {
                    onClick = [this]() {
                    if (state_->empty())
                        return;

                    const auto keyToRemove = *selected_;

                    confirmDialog_->open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                        .headerText = language->get("settings", "layoutSetting", "removeLayoutKeyConfirmHeader"),
                        .text = fmt::format(
                            fmt::runtime(
                                language->get("settings", "layoutSetting", "removeLayoutKeyConfirmText") + ": {}"
                            ),
                            keyToRemove
                        ),
                        .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
                        .onClose = [this, keyToRemove](std::optional<ConfirmDialog::Button> optButton)
                        {
                            if (!optButton)
                                return;

                            const auto button = *optButton;

                            if (button != ConfirmDialog::Button::Ok)
                                return;

                            state_.value().erase(keyToRemove);
                            state_.modifyNow();
                            onChange_();
                        },
                    });
                    },
                },
                .styleVariant = ScriptNuiComponents::StyleVariant::Transparent,
            })
        ),
        SettingBase::reset(),
        SettingBase::help()
    );
    // clang-format on
}