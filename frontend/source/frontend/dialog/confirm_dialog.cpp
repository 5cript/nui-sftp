#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/components/ui5/text.hpp>
#include <frontend/components/ui5/list.hpp>
#include <log/log.hpp>

#include <ui5/components/dialog.hpp>
#include <ui5/components/button.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

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
    Nui::Observed<State> state;
    Nui::Observed<std::string> headerText;
    Nui::Observed<std::string> text;
    Nui::Observed<Button> buttons;
    Nui::Observed<std::vector<OpenOptions::ListElement>> listItems;

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , onClose{}
        , state{}
        , headerText{}
        , text{}
        , buttons{}
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
        reference = impl_->dialog,
    }(
        section{}(
            ui5::text{
                style = "margin-bottom: 10px;"
            }(impl_->text),
            ui5::list{
                style = "max-height: 500px; overflow-y: auto;"
            }(
                impl_->listItems.map([](auto, auto const& element) {
                    const auto additionalTextState = [&element]() -> std::optional<std::string> {
                        if (element.additionalState)
                            return stateToString(element.additionalState.value());
                        return std::nullopt;
                    }();

                    return ui5::li{
                        "description"_attr = element.description,
                        "additional-text"_attr = element.additionalText,
                        "additional-text-state"_attr = additionalTextState
                    }(
                        element.text
                    );
                })
            )
        ),
        div{
            "slot"_prop = "footer",
            style="display: flex; justify-content: flex-end; width: 100%; align-items: center; gap: 10px; padding: 10px;"
        }(
            div{style = "flex: 1;"}(),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::Cancel))
                    {
                        return ui5::button{
                            "click"_event = [this](Nui::val) {
                                close(Button::Cancel);
                            }
                        }("Cancel");
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::Ok))
                    {
                        return ui5::button{
                            "click"_event = [this](Nui::val) {
                                close(Button::Ok);
                            }
                        }("Ok");
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::Yes))
                    {
                        return ui5::button{
                            "click"_event = [this](Nui::val) {
                                close(Button::Yes);
                            }
                        }("Yes");
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::No))
                    {
                        return ui5::button{
                            "click"_event = [this](Nui::val) {
                                close(Button::No);
                            }
                        }("No");
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::All))
                    {
                        return ui5::button{
                            "click"_event = [this](Nui::val) {
                                close(Button::All);
                            }
                        }("All");
                    }
                    return Nui::nil();
                }
            ),
            fragment(
                observe(impl_->buttons),
                [this]() -> Nui::ElementRenderer {
                    if (static_cast<unsigned>(impl_->buttons.value()) & static_cast<unsigned>(Button::None))
                    {
                        return ui5::button{
                            "click"_event = [this](Nui::val) {
                                close(Button::None);
                            }
                        }("None");
                    }
                    return Nui::nil();
                }
            )
        )
    );
    // clang-format on
}
