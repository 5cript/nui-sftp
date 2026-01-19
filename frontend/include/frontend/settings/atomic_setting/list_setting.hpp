#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>
#include <ids/id.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/switch.hpp>
#include <ui5/components/table.hpp>
#include <ui5/components/dialog.hpp>
#include <ui5/components/input.hpp>
#include <ui5/components/toolbar.hpp>
#include <ui5/components/toolbar_button.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/section.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/class.hpp>

template <bool Disengageable = false>
class ListSetting : public Setting<Disengageable, std::vector<std::string>>
{
  public:
    using SettingBase = Setting<Disengageable, std::vector<std::string>>;
    using ListType = std::vector<std::string>;

    using SettingBase::state_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::isEngaged;
    using SettingBase::observeEngagedToBool;

    ListSetting(LanguageObservedText helpText, std::invocable auto&& onChange, std::invocable auto&& resetAction)
        : SettingBase{
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction)
          }
        , elementIdPrefix_{Ids::generateId().id()}
    {}

    // TODO: Proper inheritance support for lists
    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::section;

        // clang-format off
        return div{}(
            ui5::dialog{
                reference = dialog_,
                "header-text"_attr = language->getObserved("settings", "general", "userInterface", "fileGridExtensionIconsAddItemText"),
            }(
                section{
                    class_ = "list-setting-add-entry-container",
                }(
                    div{}(
                        ui5::label{
                            "for"_attr = elementIdPrefix_ + "-value-input",
                            style = "margin: 10px 0 5px 0;",
                        }(language->getObserved("settings", "listSetting", "itemValue")),
                        ui5::input{
                            "id"_attr = elementIdPrefix_ + "-value-input",
                            reference = valueInput_,
                            "placeholder"_attr = language->getObserved("settings", "listSetting", "itemValue"),
                        }()
                    )
                ),
                ui5::toolbar{
                    "slot"_attr = "footer",
                }(
                    ui5::toolbar_button{
                        "design"_prop = "Emphasized",
                        "text"_prop = language->getObserved("settings", "listSetting", "addItemText"),
                        "click"_event = [this]() {
                            auto valueInput = valueInput_.lock();
                            auto dialog = dialog_.lock();

                            if (!valueInput || !dialog) {
                                Log::error("ListSetting: Unable to add new entry, dialog or inputs are not available");
                                return;
                            }

                            const auto value = valueInput->val()["value"].as<std::string>();

                            if (!value.empty()) {
                                state_->push_back(value);
                                state_.modify();
                                onChange_();
                            }

                            valueInput->val().set("value", "");
                            dialog->val().set("open", false);
                        }
                    }(),
                    ui5::toolbar_button{
                        "design"_prop = "Transparent",
                        "text"_prop = language->getObserved("cancel"),
                        "click"_event = [this]() {
                            auto valueInput = valueInput_.lock();
                            auto dialog = dialog_.lock();

                            if (valueInput)
                                valueInput->val().set("value", "");

                            if (dialog)
                                dialog->val().set("open", false);
                        }
                    }()
                )
            ),
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            div{
                class_ = "list-setting-table-container",
            }(
                ui5::table{
                    "row-action-count"_attr = 1,
                }(
                    Nui::range(state_).before(
                        ui5::table_header_row{
                            "slot"_attr = "headerRow"
                        }(
                            ui5::table_header_cell{}(language->getObserved("settings", "listSetting", "itemValue"))
                        ),
                        ui5::table_growing{
                            "mode"_attr = "Button",
                            "slot"_attr = "features",
                            "text"_attr = language->getObserved("settings", "listSetting", "addItemText"),
                            "load-more"_event = [this]() {
                                if (!isEngaged())
                                    return;
                                auto dialog = dialog_.lock();
                                if (dialog)
                                    dialog->val().set("open", true);
                            },
                        }()
                    ),
                    [this](long long index, std::string const& element) {
                        return ui5::table_row{
                            "row-key"_attr = index
                        }(
                            ui5::table_cell{}(element),
                            ui5::table_row_action{
                                "design"_prop = "Transparent",
                                "slot"_attr = "actions",
                                "icon"_prop = "delete",
                                "text"_prop = "Delete",
                                observeEngagedToBool("disabled"_prop),
                                "tooltip"_prop = language->get("settings", "deleteEntry"),
                                "click"_event = [this, index]() {
                                    state_->erase(state_->begin() + static_cast<std::size_t>(index));
                                    state_.modify();
                                    onChange_();
                                },
                            }()
                        );
                    }
                )
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    std::string elementIdPrefix_;
    std::weak_ptr<Nui::Dom::BasicElement> dialog_;
    std::weak_ptr<Nui::Dom::BasicElement> valueInput_;
};