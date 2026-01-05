#include <frontend/dialog/input_dialog.hpp>
#include <log/log.hpp>

#include <nui/rpc.hpp>
#include <ui5/components/dialog.hpp>
#include <ui5/components/button.hpp>
#include <ui5/components/input.hpp>
#include <ui5/components/label.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

struct InputDialog::Implementation
{
    std::string id;
    std::weak_ptr<Nui::Dom::BasicElement> dialog;
    std::weak_ptr<Nui::Dom::BasicElement> input;
    std::function<void(std::optional<std::string> const&)> onConfirm;
    Nui::Observed<std::string> headerText;
    Nui::Observed<bool> isPassword;
    Nui::Observed<std::string> whatFor;

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , input{}
        , onConfirm{}
    {}
};

InputDialog::InputDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{}

void InputDialog::open(OpenOptions const& options)
{
    impl_->onConfirm = options.onConfirm;
    impl_->headerText = options.headerText;
    impl_->isPassword = options.isPassword;
    impl_->whatFor = options.whatFor;
    Nui::globalEventContext.executeActiveEventsImmediately();

    if (auto input = impl_->input.lock())
    {
        input->val().set("value", "");
    }

    if (auto diag = impl_->dialog.lock(); diag)
    {
        diag->val().set("header-text", options.whatFor + ": " + options.prompt);
        diag->val().set("open", true);
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(InputDialog);

void InputDialog::closeDialog(std::optional<std::string> const& value)
{
    if (auto diag = impl_->dialog.lock(); diag)
    {
        Log::info("Closing dialog");
        diag->val().set("open", false);
    }
    if (impl_->onConfirm)
        impl_->onConfirm(value);
}

void InputDialog::confirm()
{
    Log::info("Confirm clicked");
    if (auto input = impl_->input.lock())
    {
        closeDialog(input->val()["value"].as<std::string>());
        input->val().set("value", "");
    }
    else
        closeDialog(std::nullopt);
}

Nui::ElementRenderer InputDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return ui5::dialog{
        id = "InputDialog_" + impl_->id,
        "headerText"_prop = impl_->headerText,
        reference = impl_->dialog,
    }(
        section{class_ = "prompter-form"}(
            div{}(
                ui5::label{id = "InputDialogLabel_" + impl_->id, "for"_prop = "InputDialogInput_" + impl_->id}(
                    observe(impl_->whatFor),
                    [this](){
                        return impl_->whatFor.value() + ":";
                    }
                ),
                ui5::input{
                    id = "InputDialogInput_" + impl_->id,
                    "type"_prop = observe(impl_->isPassword).generate([this](){
                        if (impl_->isPassword.value())
                            return "Password";
                        return "Text";
                    }),
                    reference = impl_->input,
                    "keyup"_event = [this](Nui::val event){
                        if (event["key"].as<std::string>() == "Enter")
                            confirm();
                    }
                }()
            )
        ),
        div{
            "slot"_attr = "footer",
            style="display: flex; justify-content: flex-end; width: 100%; align-items: center; gap: 10px"
        }(
            div{style = "flex: 1;"}(),
            ui5::button{
                "design"_prop = "Emphasized",
                "click"_event = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    closeDialog(std::nullopt);
                }
            }("Cancel"),
            ui5::button{
                "design"_prop = "Emphasized",
                "click"_event = [this](Nui::val){
                    confirm();
                }
            }("Ok")
        )
    );
    // clang-format on
}
