#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>
#include <persistence/state_holder.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/resizeable_table.hpp>
#include <script-nui-components/switch.hpp>

#include <script-nui-components/dialog.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>
#include <nui/frontend/api/keyboard_event.hpp>

#include <fmt/format.h>

#include <algorithm>

using namespace std::string_literals;
namespace Snc = ScriptNuiComponents;

struct ConfirmDialog::Implementation
{
    std::string id;
    Persistence::StateHolder* stateHolder;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    Snc::ResizableTable table;
    Nui::Observed<std::string> text;
    Nui::Observed<bool> listItemsPresent{false};
    Nui::Observed<bool> showNeverAgain{false};
    Nui::Observed<bool> neverAgainChecked{false};
    std::string currentNeverShowAgainId;

    Implementation(std::string id, Persistence::StateHolder& stateHolder)
        : id{std::move(id)}
        , stateHolder{&stateHolder}
        , dialog{}
        , table{
              Snc::ResizableTable::HeaderRow{
                  // Resizeable with dynamic width:
                  Snc::ResizableTable::HeaderTableCell{language->get("confirmDialog", "items"), 600, false},
              },
              // no footer
              std::nullopt,
              // no add feature
              std::nullopt
          }
    {}
};

ConfirmDialog::ConfirmDialog(std::string id, Persistence::StateHolder& stateHolder)
    : impl_{std::make_unique<Implementation>(std::move(id), stateHolder)}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, dialogBody());
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(ConfirmDialog);

void ConfirmDialog::open(OpenOptions const& options)
{
    if (options.neverShowAgainId.has_value())
    {
        auto const& neverShow = impl_->stateHolder->stateCache().uiOptions.neverShowAgainDialogs;
        if (neverShow.find(*options.neverShowAgainId) != neverShow.end())
        {
            options.onClose(std::nullopt);
            return;
        }
        impl_->currentNeverShowAgainId = *options.neverShowAgainId;
        impl_->neverAgainChecked = false;
        impl_->showNeverAgain = true;
    }
    else
    {
        impl_->currentNeverShowAgainId.clear();
        impl_->showNeverAgain = false;
    }

    impl_->listItemsPresent = !options.listItems.empty();
    impl_->table.clear();
    // Each row is a nested reactive range plus DOM nodes, so a multi-thousand
    // item drop (bulk transfer confirmation) would build that many rows on the
    // WASM thread and stall. The list is purely informational; the action still
    // runs on the full set, so cap the rendered rows and summarize the rest.
    constexpr std::size_t maxListedRows = 200;
    const auto totalRows = options.listItems.size();
    const auto shownRows = std::min(totalRows, maxListedRows);
    for (std::size_t idx = 0; idx < shownRows; ++idx)
        impl_->table.addRow({options.listItems[idx].text});
    if (totalRows > shownRows)
        impl_->table.addRow({fmt::format("... and {} more", totalRows - shownRows)});
    impl_->text = options.text;
    impl_->dialog->open(
        {.styleVariant = options.styleVariant,
            .headerText = options.headerText,
            .buttons = options.buttons,
            .initialFocus = options.focusButton,
            .onClose = options.onClose,
            .modal = true,
            .mayCloseWithoutButton = (options.buttons & ScriptNuiComponents::Dialog::Button::Cancel) !=
                ScriptNuiComponents::Dialog::Button::Unknown}
    );
}

Nui::ElementRenderer ConfirmDialog::dialogBody()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return section{
        class_ = "confirm-dialog",
    }(
        textarea{
            class_ = "confirm-dialog-text",
            "readonly"_attr = true,
            "rows"_attr = 10,
            style = "width: 100%; height: 200px; margin-bottom: 10px; resize: auto;"
        }(impl_->text),
        div{
            style = "margin-bottom: 10px;"
        }(
            observe(impl_->listItemsPresent),
            [this](bool present) -> Nui::ElementRenderer {
                if (!present)
                    return Nui::nil();
                return impl_->table({style = "max-height: 200px;"});
            }
        ),
        div{
            style = "display: flex; align-items: center; gap: 8px; margin-top: 10px;"
        }(
            observe(impl_->showNeverAgain),
            [this](bool show) -> Nui::ElementRenderer {
                if (!show)
                    return Nui::nil();
                return div{
                    style = "display: flex; align-items: center; gap: 8px;"
                }(
                    Snc::switch_({
                        .isChecked = impl_->neverAgainChecked,
                        .onChange = [this](bool checked, auto const&) {
                            impl_->neverAgainChecked = checked;
                            impl_->stateHolder->loadModifySave(
                                [checked, id = impl_->currentNeverShowAgainId](Persistence::State& state) {
                                    if (checked)
                                        state.uiOptions.neverShowAgainDialogs.insert(id);
                                    else
                                        state.uiOptions.neverShowAgainDialogs.erase(id);
                                }
                            );
                        },
                        .dontUpdateValue = true,
                    }),
                    div{
                        style = "font-size: 14px; color: var(--muted);"
                    }(language->getObserved("confirmDialog", "neverShowAgain"))
                );
            }
        )
    );
    // clang-format on
}

Nui::ElementRenderer ConfirmDialog::operator()()
{
    return (*impl_->dialog)();
}