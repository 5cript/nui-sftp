#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <frontend/dialog/input_dialog.hpp>
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
class ListSetting : public Setting<Disengageable, std::vector<std::string>>
{
  public:
    using SettingBase = Setting<Disengageable, std::vector<std::string>>;
    using ListType = std::vector<std::string>;

    using SettingBase::state_;
    using SettingBase::stateWithInheritance_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::isEngaged;
    using SettingBase::observeEngagedToBool;

    ListSetting(
        LanguageObservedText helpText,
        InputDialog& inputDialog,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction
    )
        : SettingBase{
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction)
          }
        , elementIdPrefix_{Ids::generateId().id()}
        , inputDialog_{&inputDialog}
        , table_{std::make_shared<ScriptNuiComponents::ResizableTable>(
              ScriptNuiComponents::ResizableTable::HeaderRow{
                  {std::string{language->get("settings", "listSettings", "valueName")}},
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
                          return;
                      inputDialog_->open(
                          InputDialog::OpenOptions{
                              .whatFor = "",
                              .prompt = language->get("settings", "listSettings", "addItemPrompt"),
                              .headerText = language->get("settings", "listSettings", "addItemHeader"),
                              .onConfirm = [this](std::optional<std::string> const& result)
                              {
                                  if (!result) // The dialog was closed without confirming
                                      return;
                                  if (result->empty()) // The dialog was confirmed with an empty value
                                      return;

                                  auto currentList = state_.value();
                                  currentList.push_back(*result);
                                  this->value(currentList);
                                  onChange_();
                              },
                          }
                      );
                  },
                  .addNewEntryText = language->get("settings", "listSettings", "addItemText")
              }
          )}
        , selfAlive_{std::make_shared<bool>(true)}
    {
        Nui::listen(
            stateWithInheritance_,
            [this,
                weakTable = std::weak_ptr<ScriptNuiComponents::ResizableTable>(table_),
                selfAlive = selfAlive_](std::vector<std::string> const& list)
            {
                using namespace ScriptNuiComponents;
                using namespace std::string_literals;

                // Should realistically never occur, but stateWithInheritance_ survives this derived class, lets not
                // make useAfterFree bugs.
                if (!*selfAlive)
                {
                    Nui::WebApi::Console::error("ListSetting: Receive update for destroyed ListSetting, ignoring.");
                    return;
                }

                auto table = weakTable.lock();
                if (!table)
                {
                    Nui::WebApi::Console::error("ListSetting: Table component was destroyed, cannot update.");
                    return;
                }

                std::vector<ResizableTable::TableRow> rows;
                for (const auto& element : list)
                {
                    rows.push_back(
                        ResizableTable::TableRow{
                            element,
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
                                                state_.value().erase(state_.value().begin() + controller->row());
                                                controller->remove();
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

    ~ListSetting()
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
    std::string elementIdPrefix_;
    InputDialog* inputDialog_;
    std::shared_ptr<ScriptNuiComponents::ResizableTable> table_{};

    // Keep last:
    std::shared_ptr<bool> selfAlive_;
};