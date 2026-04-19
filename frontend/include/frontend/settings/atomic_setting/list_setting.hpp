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

template <bool Disengageable = false, template <typename...> typename ListTypeT = std::vector>
class ListSetting : public Setting<Disengageable, ListTypeT<std::string>>
{
  public:
    using ListType = ListTypeT<std::string>;
    using SettingBase = Setting<Disengageable, ListTypeT<std::string>>;

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
                      openAddItemDialog();
                  },
                  .addNewEntryText = language->get("settings", "listSettings", "addItemText")
              }
          )}
    {
        setupStateListener();
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
            div{class_ = "setting-table-container"}(
                (*table_)(
                    {observeEngagedToBool(disabled)}
                )
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    void openAddItemDialog()
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
                    if (!result || result->empty())
                        return;
                    auto currentList = state_.value();
                    if constexpr (requires(ListType& c, std::string s) { c.push_back(s); })
                        currentList.push_back(*result);
                    else
                        currentList.insert(*result);
                    this->value(currentList);
                    onChange_();
                },
            }
        );
    }

    ScriptNuiComponents::ResizableTable::TableCell makeDeleteCell()
    {
        using namespace ScriptNuiComponents;
        return ResizableTable::TableCell{
            [this](std::unique_ptr<ResizableTable::ISelfController> controller) -> Nui::ElementRenderer
            {
                // std::function must be copiable
                std::shared_ptr<ResizableTable::ISelfController> sharedController = std::move(controller);
                return button({
                    .icon = GeneratedSvgs::delete_(),
                    .attributes = {
                        Nui::Attributes::onClick = [this, controller = std::move(sharedController)](auto const&)
                        {
                            if constexpr (requires(ListType& c) { c.begin() + 0; })
                                state_.value().erase(state_.value().begin() + controller->row());
                            else
                                state_.value().erase(std::next(state_.value().begin(), controller->row()));
                            controller->remove();
                            onChange_();
                        }
                    },
                });
            }
        };
    }

    std::vector<ScriptNuiComponents::ResizableTable::TableRow> buildTableRows(ListType const& list)
    {
        using namespace ScriptNuiComponents;
        std::vector<ResizableTable::TableRow> rows;
        rows.reserve(list.size());
        for (const auto& element : list)
            rows.push_back(ResizableTable::TableRow{element, makeDeleteCell()});
        return rows;
    }

    void setupStateListener()
    {
        stateListener_ = Nui::smartListen(
            stateWithInheritance_,
            [this](ListType const& list)
            {
                table_->setRows(buildTableRows(list));
            }
        );
    }

    std::string elementIdPrefix_;
    InputDialog* inputDialog_;
    std::shared_ptr<ScriptNuiComponents::ResizableTable> table_{};
    Nui::ListenRemover<Nui::Observed<ListType>> stateListener_;
};