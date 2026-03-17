#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <frontend/components/ui5/text.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/resizeable_table.hpp>

#include <ui5/components/text_area.hpp>

#include <script-nui-components/dialog.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>
#include <nui/frontend/api/keyboard_event.hpp>

using namespace std::string_literals;
namespace Snc = ScriptNuiComponents;

struct ConfirmDialog::Implementation
{
    std::string id;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    std::function<void(Button)> onClose;
    Snc::ResizableTable table;
    // Nui::Observed<State> state;
    Nui::Observed<std::string> text;
    Nui::Observed<std::vector<OpenOptions::ListElement>> listItems;

    Implementation(ConfirmDialog& owner, std::string id)
        : id{std::move(id)}
        , dialog{}
        , onClose{}
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

ConfirmDialog::ConfirmDialog(std::string id)
    : impl_{std::make_unique<Implementation>(*this, std::move(id))}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, dialogBody());
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(ConfirmDialog);

void ConfirmDialog::open(OpenOptions const& options)
{
    impl_->listItems = options.listItems;
    impl_->table.clear();
    for (const auto& item : options.listItems)
    {
        impl_->table.addRow({item.text});
    }
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
    return section{}(
        ui5::textarea{
            "value"_prop = impl_->text,
            "readonly"_prop = true,
            "growing"_prop = true,
            "growMaxRows"_prop = 25,
            style = "width: 100%; height: 200px; margin-bottom: 10px;"
        }(),
        div{
            style = "margin-bottom: 10px;"
        }(
            observe(impl_->listItems),
            [this]() -> Nui::ElementRenderer {
                if (impl_->listItems.empty())
                    return Nui::nil();
                return impl_->table({style = "max-height: 200px;"});
            }
        )
    );
    // clang-format on
}

Nui::ElementRenderer ConfirmDialog::operator()()
{
    return (*impl_->dialog)();
}