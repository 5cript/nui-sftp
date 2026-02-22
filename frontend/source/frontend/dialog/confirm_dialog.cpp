#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/components/ui5/text.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/resizeable_table.hpp>

#include <ui5/components/dialog.hpp>
#include <ui5/components/text_area.hpp>
#include <frontend/components/ui5/list.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>
#include <nui/frontend/api/keyboard_event.hpp>

using namespace std::string_literals;
namespace Snc = ScriptNuiComponents;

namespace
{
    std::string stateToString(ConfirmDialog::State state)
    {
        switch (state)
        {
            case ConfirmDialog::State::Positive:
                return "Positive";
            case ConfirmDialog::State::Critical:
                return "Critical";
            case ConfirmDialog::State::Negative:
                return "Negative";
            case ConfirmDialog::State::Information:
                return "Information";
            default:
                return "None";
        }
    }
}

struct ConfirmDialog::Implementation
{
    std::string id;
    std::weak_ptr<Nui::Dom::BasicElement> dialog;
    std::function<void(Button)> onClose;
    Snc::ResizableTable table;
    Nui::Observed<State> state;
    Nui::Observed<std::string> headerText;
    // Nui::Observed<std::vector<std::string>> textLines;
    Nui::Observed<std::string> text;
    Nui::Observed<Button> buttons;
    Nui::Observed<std::optional<Button>> focusButton;
    Nui::Observed<std::vector<OpenOptions::ListElement>> listItems;
    std::weak_ptr<Nui::Dom::BasicElement> footer{};

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , onClose{}
        , table{
          Snc::ResizableTable::HeaderRow{
              // Resizeable with dynamic width:
              Snc::ResizableTable::HeaderTableCell{
                language->get("confirmDialog", "items"),
                600,
                false
            },
          },
          // no footer
          std::nullopt,
          // no add feature
          std::nullopt
        }
        , state{}
        , headerText{}
        , text{}
        , buttons{}
        , focusButton{}
        , listItems{}
    {}
};

ConfirmDialog::ConfirmDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{}

void ConfirmDialog::open(OpenOptions const& options)
{
    impl_->onClose = options.onClose;
    impl_->state = options.state;
    impl_->headerText = options.headerText;
    impl_->text = options.text;
    impl_->buttons = options.buttons;
    impl_->listItems = options.listItems;
    impl_->focusButton = options.focusButton;
    impl_->table.clear();
    for (const auto& item : options.listItems)
    {
        impl_->table.addRow({item.text});
    }
    Nui::globalEventContext.executeActiveEventsImmediately();

    if (auto diag = impl_->dialog.lock(); diag)
    {
        diag->val().set("header-text", options.headerText);
        diag->val().set("open", true);
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(ConfirmDialog);

void ConfirmDialog::close(Button button)
{
    if (auto diag = impl_->dialog.lock(); diag)
    {
        Log::info("Closing dialog");
        diag->val().set("open", false);
    }
    if (impl_->onClose)
        impl_->onClose(button);
}

Nui::ElementRenderer ConfirmDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return ui5::dialog{
        id = "ConfirmDialog_" + impl_->id,
        "state"_prop = observe(impl_->state).generate([this](){
            return stateToString(impl_->state.value());
        }),
        "headerText"_prop = impl_->headerText,
        "initialFocus"_prop = observe(impl_->focusButton).generate([this]() -> std::string {
            if (!impl_->focusButton.value())
                return "";
            const auto button = impl_->focusButton->value_or(Button::Ok);
            switch (button)
            {
                case Button::Ok:
                    return impl_->id + "_ok";
                case Button::Cancel:
                    return impl_->id + "_cancel";
                case Button::Yes:
                    return impl_->id + "_yes";
                case Button::No:
                    return impl_->id + "_no";
                case Button::All:
                    return impl_->id + "_all";
                case Button::None:
                    return impl_->id + "_none";
                default:
                    return impl_->id + "_ok";
            }
        }),
        reference = impl_->dialog,
    }(
        section{}(
            ui5::textarea{
                "value"_prop = impl_->text,
                "readonly"_prop = true,
                "growing"_prop = true,
                "growMaxRows"_prop = 25,
                style = "width: 100%; height: 200px; margin-bottom: 10px;",
                "state"_prop = observe(impl_->state).generate([this](){
                    return stateToString(impl_->state.value());
                }),
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
        ),
        div{
            style="display: inline-grid; width: 100%; grid-auto-columns: 1fr; gap: 10px;",
            tabIndex = 0,
            reference = [this](std::weak_ptr<Nui::Dom::BasicElement> footer) {
                impl_->footer = std::move(footer);
            },
            "keydown"_event = [](Nui::WebApi::KeyboardEvent event) {
                // clang-format on
                if (event.key() == "ArrowLeft" || event.key() == "ArrowUp" || event.key() == "ArrowRight" ||
                    event.key() == "ArrowDown")
                {
                    event.preventDefault();
                    event.stopPropagation();
                }
                if (event.key() == "ArrowLeft" || event.key() == "ArrowUp")
                {
                    auto button = event.currentTarget().call<Nui::val>("querySelector", "button:focus"s);

                    if (button.isUndefined() || button.isNull())
                        return;

                    auto previous = button["previousElementSibling"];

                    if (previous.isUndefined() || previous.isNull())
                        previous = event.currentTarget().call<Nui::val>("querySelector", "button:last-child"s);

                    if (previous.isUndefined() || previous.isNull())
                        return;

                    previous.call<void>("focus");
                    return;
                }
                if (event.key() == "ArrowRight" || event.key() == "ArrowDown")
                {
                    auto button = event.currentTarget().call<Nui::val>("querySelector", "button:focus"s);

                    if (button.isUndefined() || button.isNull())
                        return;

                    auto next = button["nextElementSibling"];

                    if (next.isUndefined() || next.isNull())
                        next = event.currentTarget().call<Nui::val>("querySelector", "button"s);

                    if (next.isUndefined() || next.isNull())
                        return;

                    next.call<void>("focus");
                }
                // clang-format off
            }
        }(
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::Cancel))
                    {
                        return Snc::button({
                            .text = language->get("confirmDialog", "cancel"),
                            .attributes = {
                                id = impl_->id + "_cancel",
                                "click"_event = [this](Nui::val) {
                                    close(Button::Cancel);
                                },
                                style = "grid-row: 1",
                            },
                            .styleVariant = Snc::StyleVariant::Regular
                        });
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::Ok))
                    {
                        return Snc::button({
                            .text = language->get("confirmDialog", "ok"),
                            .attributes = {
                                id = impl_->id + "_ok",
                                "click"_event = [this](Nui::val) {
                                    close(Button::Ok);
                                },
                                style = "grid-row: 1"
                            },
                            .styleVariant = Snc::StyleVariant::Regular
                        });
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::Yes))
                    {
                        return Snc::button({
                            .text = language->get("confirmDialog", "yes"),
                            .attributes = {
                                id = impl_->id + "_yes",
                                "click"_event = [this](Nui::val) {
                                    close(Button::Yes);
                                },
                                style = "grid-row: 1"
                            },
                            .styleVariant = Snc::StyleVariant::Regular
                        });
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::No))
                    {
                        return Snc::button({
                            .text = language->get("confirmDialog", "no"),
                            .attributes = {
                                id = impl_->id + "_no",
                                "click"_event = [this](Nui::val) {
                                    close(Button::No);
                                },
                                style = "grid-row: 1"
                            },
                            .styleVariant = Snc::StyleVariant::Regular
                        });
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::All))
                    {
                        return Snc::button({
                            .text = language->get("confirmDialog", "all"),
                            .attributes = {
                                id = impl_->id + "_all",
                                "click"_event = [this](Nui::val) {
                                    close(Button::All);
                                },
                                style = "grid-row: 1"
                            },
                            .styleVariant = Snc::StyleVariant::Danger
                        });
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::None))
                    {
                        return Snc::button({
                            .text = language->get("confirmDialog", "none"),
                            .attributes = {
                                id = impl_->id + "_none",
                                "click"_event = [this](Nui::val) {
                                    close(Button::None);
                                },
                                style = "grid-row: 1"
                            },
                            .styleVariant = Snc::StyleVariant::Regular
                        });
                    }
                    return Nui::nil();
                }
            )
        )
    );
    // clang-format on
}
