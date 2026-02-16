#include <frontend/dialog/multi_input_dialog.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/text_input.hpp>
#include <script-nui-components/button.hpp>

#include <ui5/components/dialog.hpp>
#include <ui5/components/button.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

struct MultiInputDialog::Implementation
{
    std::string id;
    std::weak_ptr<Nui::Dom::BasicElement> dialog;
    std::function<void(std::optional<std::unordered_map<std::string, std::string>> const&)> onConfirm;
    Nui::Observed<std::vector<InputField>> inputFields{};
    Nui::Observed<std::string> headerText{};
    std::unordered_map<std::string, std::string> values;

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , onConfirm{}
    {}
};

MultiInputDialog::MultiInputDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{}

void MultiInputDialog::open(OpenOptions const& options)
{
    impl_->onConfirm = options.onConfirm;
    impl_->headerText = options.headerText;
    impl_->inputFields = options.inputFields;
    Nui::globalEventContext.executeActiveEventsImmediately();

    if (auto diag = impl_->dialog.lock(); diag)
    {
        diag->val().set("header-text", options.headerText);
        diag->val().set("open", true);
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(MultiInputDialog);

void MultiInputDialog::closeDialog(std::optional<std::unordered_map<std::string, std::string>> const& values)
{
    if (auto diag = impl_->dialog.lock(); diag)
    {
        Log::info("Closing dialog");
        diag->val().set("open", false);
    }
    if (impl_->onConfirm)
        impl_->onConfirm(values);
}

void MultiInputDialog::confirm()
{
    closeDialog(impl_->values);
    impl_->values.clear();
}

Nui::ElementRenderer MultiInputDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return ui5::dialog{
        id = "MultiInputDialog_" + impl_->id,
        "headerText"_prop = impl_->headerText,
        reference = impl_->dialog,
    }(
        section{class_ = "prompter-form"}(
            Nui::range(impl_->inputFields),
            [this](long long, InputField const& field) -> Nui::ElementRenderer
            {
                return div{class_ = "prompter-form-field"}(
                    span{}(field.label),
                    Snc::textInput(
                        Snc::TextInputOptions<std::string>{
                            .onChange = [this, field](std::string const& value, auto const&){
                                impl_->values[field.key] = value;
                            },
                            .dontUpdateValue = true
                        }
                    )
                );
            }
        ),
        div{
            "slot"_attr = "footer",
            style="display: flex; justify-content: flex-end; width: 100%; align-items: center; gap: 10px"
        }(
            div{style = "flex: 1;"}(),
            Snc::button(
                Snc::ButtonOptions{
                    .text = "Cancel",
                    .attributes = std::vector<Nui::Attribute>{
                        onClick = [this](Nui::WebApi::Event event){
                            event.stopPropagation();
                            closeDialog(std::nullopt);
                        }
                    },
                    .styleVariant = Snc::StyleVariant::Regular,
                }
            ),
            Snc::button(
                Snc::ButtonOptions{
                    .text = "OK",
                    .attributes = std::vector<Nui::Attribute>{
                        onClick = [this](Nui::WebApi::Event event){
                            event.stopPropagation();
                            confirm();
                        }
                    },
                    .styleVariant = Snc::StyleVariant::Primary,
                }
            )
        )
    );
    // clang-format on
}
