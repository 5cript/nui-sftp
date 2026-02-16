#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>
#include <ids/id.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/switch.hpp>
#include <ui5/components/table.hpp>
#include <ui5/components/dialog.hpp>
#include <ui5/components/input.hpp>
#include <ui5/components/button.hpp>
#include <ui5/components/toolbar.hpp>
#include <ui5/components/toolbar_button.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/span.hpp>
#include <nui/frontend/elements/section.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/class.hpp>

template <bool Disengageable = false>
class MapSetting : public Setting<Disengageable, std::map<std::string, std::string>>
{
  public:
    using SettingBase = Setting<Disengageable, std::map<std::string, std::string>>;
    using MapType = std::map<std::string, std::string>;

    using SettingBase::state_;
    using SettingBase::inheritedState_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::isEngaged;
    using SettingBase::engaged_;

    MapSetting(
        LanguageObservedText helpText,
        MultiInputDialog& multiInputDialog,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction
    )
        : SettingBase{
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction)
          }
        , elementIdPrefix_{Ids::generateId().id()}
        , multiInputDialog_{&multiInputDialog}
    {}

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        using Nui::Elements::section;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            div{
                class_ = "map-setting-table-container"
            }(
                observe(engaged_),
                [this]() -> Nui::ElementRenderer {
                    if (isEngaged())
                        return tableContainer();
                    return inheritedDisplay();
                }
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    void openDialog()
    {
        multiInputDialog_->open(
            MultiInputDialog::OpenOptions{
                .headerText = language->get("settings", "mapSettings", "addEntryHeader"),
                .inputFields =
                    {
                        MultiInputDialog::InputField{
                            .key = "key",
                            .label = language->get("settings", "mapSettings", "keyInputLabel"),
                            .placeholder = language->get("settings", "mapSettings", "keyInputPlaceholder"),
                        },
                        MultiInputDialog::InputField{
                            .key = "value",
                            .label = language->get("settings", "mapSettings", "valueInputLabel"),
                            .placeholder = language->get("settings", "mapSettings", "valueInputPlaceholder"),
                        },
                    },
                .onConfirm = [this](std::optional<std::unordered_map<std::string, std::string>> const& result)
                {
                    if (!result)
                        return;
                    if (result->empty())
                        return;

                    auto& mapState = *state_;
                    for (const auto& [key, value] : *result)
                    {
                        mapState[key] = value;
                    }
                    state_.modify();
                    onChange_();
                }
            }
        );
    }

    Nui::ElementRenderer inheritedDisplay()
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        using Nui::Elements::section;

        if (!*inheritedState_)
        {
            // clang-format off
            return ui5::table{
                "row-action-count"_attr = 1, class_ = "multi-setting-disabled-table"
            }(
                ui5::table_header_row{
                    "slot"_attr = "headerRow"
                }(
                    ui5::table_header_cell{}(language->getObserved("settings", "mapSettings", "keyName")),
                    ui5::table_header_cell{}(language->getObserved("settings", "mapSettings", "valueName"))
                )
            );
            // clang-format on
        }

        // clang-format off
        return ui5::table{
            "row-action-count"_attr = 1,
            class_ = "multi-setting-disabled-table"
        }(
            Nui::range(**inheritedState_).before(
                ui5::table_header_row{
                    "slot"_attr = "headerRow"
                }(
                    ui5::table_header_cell{}(
                        span{
                            style = "margin-right: 8px;"
                        }(language->getObserved("settings", "mapSettings", "keyName")),
                        ui5::button{
                            class_ = "multi-setting-key-add-button",
                            "design"_prop = "Transparent",
                            "icon"_prop = "add",
                            "click"_event = [this]() {
                                if (!isEngaged())
                                    return;
                                openDialog();
                            }
                        }()
                    ),
                    ui5::table_header_cell{}(language->getObserved("settings", "mapSettings", "valueName"))
                )
            ),
            [this](long long index, std::pair<std::string, std::string> const& element) {
                return ui5::table_row{
                    "row-key"_attr = index
                }(
                    ui5::table_cell{}(element.first),
                    ui5::table_cell{}(element.second),
                    ui5::table_row_action{
                        "design"_prop = "Transparent",
                        "slot"_attr = "actions",
                        "icon"_prop = "delete",
                        "text"_prop = "Delete",
                        "tooltip"_prop = language->get("settings", "mapSettings", "deleteEntry"),
                        "click"_event = [this, key = element.first]() {
                            state_->erase(key);
                            state_.modify();
                            onChange_();
                        },
                    }()
                );
            }
        );
        // clang-format on
    }

    Nui::ElementRenderer tableContainer()
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        using Nui::Elements::section;

        // clang-format off
        return ui5::table{
            "row-action-count"_attr = 1,
        }(
            Nui::range(state_).before(
                ui5::table_header_row{
                    "slot"_attr = "headerRow"
                }(
                    ui5::table_header_cell{}(
                        span{
                            style = "margin-right: 8px;"
                        }(language->getObserved("settings", "mapSettings", "keyName")),
                        ui5::button{
                            class_ = "multi-setting-key-add-button",
                            "design"_prop = "Transparent",
                            "icon"_prop = "add",
                            "click"_event = [this]() {
                                if (!isEngaged())
                                    return;
                                openDialog();
                            }
                        }()
                    ),
                    ui5::table_header_cell{}(language->getObserved("settings", "mapSettings", "valueName"))
                )
            ),
            [this](long long index, std::pair<std::string, std::string> const& element) {
                return ui5::table_row{
                    "row-key"_attr = index
                }(
                    ui5::table_cell{}(element.first),
                    ui5::table_cell{}(element.second),
                    ui5::table_row_action{
                        "design"_prop = "Transparent",
                        "slot"_attr = "actions",
                        "icon"_prop = "delete",
                        "text"_prop = "Delete",
                        "tooltip"_prop = language->get("settings", "mapSettings", "deleteEntry"),
                        "click"_event = [this, key = element.first]() {
                            state_->erase(key);
                            state_.modify();
                            onChange_();
                        },
                    }()
                );
            }
        );
        // clang-format on
    }

  private:
    std::string elementIdPrefix_;
    MultiInputDialog* multiInputDialog_;
    std::weak_ptr<Nui::Dom::BasicElement> keyInput_;
    std::weak_ptr<Nui::Dom::BasicElement> valueInput_;
};