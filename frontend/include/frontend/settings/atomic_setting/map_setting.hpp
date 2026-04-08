#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>
#include <ids/id.hpp>

#include <script-nui-components/resizeable_table.hpp>
#include <frontend/svgs/delete.hpp>

#include <nui/event_system/listen.hpp>
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
    using SettingBase::stateWithInheritance_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::isEngaged;
    using SettingBase::observeEngagedToBool;
    using SettingBase::updateStateWithInheritance;

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
        , table_{std::make_shared<ScriptNuiComponents::ResizableTable>(
              ScriptNuiComponents::ResizableTable::HeaderRow{
                  {std::string{language->get("settings", "mapSettings", "keyName")}},
                  {std::string{language->get("settings", "mapSettings", "valueName")}},
                  // Delete Item Row:
                  ScriptNuiComponents::ResizableTable::HeaderTableCell{
                      .content = std::string{},
                      .initialWidth = 40,
                      .resizeable = false
                  }
              },
              // No Footer required.
              std::nullopt,
              ScriptNuiComponents::ResizableTable::AddFeature{
                  .onAdd =
                      [this](auto const&)
                  {
                      if (!isEngaged())
                      {
                          return;
                      }
                      return openDialog();
                  },
                  .addNewEntryText = language->get("settings", "listSettings", "addItemText")
              }
          )}
        , selfAlive_{std::make_shared<bool>(true)}
    {
        stateWithInheritanceListen_ = Nui::smartListen(
            stateWithInheritance_,
            [this,
                weakTable = std::weak_ptr<ScriptNuiComponents::ResizableTable>(table_),
                selfAlive = selfAlive_](std::map<std::string, std::string> const& map)
            {
                using namespace ScriptNuiComponents;
                using namespace std::string_literals;

                // Should realistically never occur, but stateWithInheritance_ survives this derived class, lets not
                // make useAfterFree bugs.
                if (!*selfAlive)
                    return;

                auto table = weakTable.lock();
                if (!table)
                    return;

                std::vector<ResizableTable::TableRow> rows;
                for (const auto& [key, value] : map)
                {
                    rows.push_back(
                        ResizableTable::TableRow{
                            key,
                            value,
                            ResizableTable::TableCell{
                                [this](std::unique_ptr<ResizableTable::ISelfController> controller)
                                    -> Nui::ElementRenderer
                                {
                                    // std::function must be copiable
                                    std::shared_ptr<ResizableTable::ISelfController> sharedController =
                                        std::move(controller);

                                    return button({
                                        .icon = GeneratedSvgs::delete_(),
                                        .attributes = {
                                            Nui::Attributes::onClick =
                                                [this, controller = std::move(sharedController)](auto const&)
                                            {
                                                const auto key = std::get<std::string>(controller->rowData()[0]);
                                                state_->erase(key);
                                                updateStateWithInheritance();
                                                state_.modifyNow();
                                                onChange_();
                                            }
                                        },
                                    });
                                }
                            }
                        }
                    );
                }
                table->setRows(rows);
            }
        );
    }

    ~MapSetting()
    {
        *selfAlive_ = false;
    }

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        using Nui::Elements::section;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            (*table_)(
                {observeEngagedToBool(disabled)}
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
                    Nui::WebApi::Console::log(
                        "Dialog confirmed with result: {}", result ? "value present" : "no value"
                    );
                    if (!result)
                    {
                        Nui::WebApi::Console::log("Dialog was cancelled, ignoring.");
                        return;
                    }
                    if (result->empty())
                    {
                        Nui::WebApi::Console::log("Dialog confirmed without input, ignoring.");
                        return;
                    }

                    const auto keyIter = result->find("key");
                    const auto valueIter = result->find("value");
                    if (keyIter == result->end() || valueIter == result->end())
                    {
                        Nui::WebApi::Console::log(
                            "Result from MultiInputDialog did not contain expected keys, ignoring."
                        );
                        return;
                    }

                    const auto key = keyIter->second;
                    const auto value = valueIter->second;

                    if (key.empty())
                    {
                        Nui::WebApi::Console::log("Tried to add entry with empty key, ignoring.");
                        return;
                    }

                    Nui::WebApi::Console::log("Adding entry: {}={}", key, value);
                    state_.value().emplace(key, value);
                    updateStateWithInheritance();
                    state_.modifyNow();
                    onChange_();
                }
            }
        );
    }

  private:
    std::string elementIdPrefix_;
    MultiInputDialog* multiInputDialog_;
    std::shared_ptr<ScriptNuiComponents::ResizableTable> table_{};

    // Keep last:
    Nui::ListenRemover<decltype(stateWithInheritance_)> stateWithInheritanceListen_{};
    std::shared_ptr<bool> selfAlive_;
};