#include <frontend/dialog/multi_input_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

struct MultiInputDialog::Implementation
{
    std::string id;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    std::function<void(std::optional<std::unordered_map<std::string, std::string>> const&)> onConfirm;
    Nui::Observed<std::vector<InputField>> inputFields{};
    std::unordered_map<std::string, std::string> values;

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , onConfirm{}
    {}
};

MultiInputDialog::MultiInputDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, dialogBody());
}

Nui::ElementRenderer MultiInputDialog::dialogBody()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return section{class_ = "multi-input-dialog-section"}(
        Nui::range(impl_->inputFields),
        [this](long long i, InputField const& field) -> Nui::ElementRenderer
        {
            const auto fieldsSize = static_cast<long long>(impl_->inputFields.value().size());

            return div{class_ = "multi-input-dialog-field"}(
                span{}(field.label),
                Snc::textInput({
                    .value = "",
                    .attributes = {
                        "data-key"_attr = field.key,
                        id = fmt::format("MultiInputDialogInput_{}_{}", impl_->id, field.key),
                        onKeyDown = [this, i, fieldsSize, key = field.key](Nui::WebApi::KeyboardEvent event)
                        {
                            if (event.key() != "Enter")
                                return;

                            event.preventDefault();

                            impl_->values[key] = event.target()["value"].as<std::string>();

                            if (i == fieldsSize - 1)
                            {
                                impl_->dialog->close();
                                impl_->onConfirm(impl_->values);
                                impl_->values.clear();
                                return;
                            }
                            // Focus next input field
                            if (i < fieldsSize - 1)
                            {
                                const auto nextFieldId = fmt::format(
                                    "MultiInputDialogInput_{}_{}", impl_->id, impl_->inputFields.value()[i + 1].key);
                                auto doc = Nui::val::global("document");
                                if (auto nextInput = doc.call<Nui::val>("getElementById", nextFieldId);
                                    !nextInput.isNull() && !nextInput.isUndefined())
                                {
                                    nextInput.call<void>("focus");
                                }
                                else
                                {
                                    Nui::WebApi::Console::error(
                                        "Could not find next input field with id: {}", nextFieldId);
                                }
                            }
                        }
                    },
                    .onChange = [this, field](std::string const& value, auto const&) {
                        Nui::WebApi::Console::log("Input changed for field {}: {}", field.key, value);
                        impl_->values[field.key] = value;
                    },
                    .dontUpdateValue = true,
                })
            );
        }
    );
    // clang-format on
}

void MultiInputDialog::open(OpenOptions const& options)
{
    namespace Snc = ScriptNuiComponents;

    impl_->onConfirm = options.onConfirm;
    impl_->inputFields = options.inputFields;
    impl_->values.clear();
    Nui::globalEventContext.executeActiveEventsImmediately();

    const std::string initialFocus = options.inputFields.empty()
        ? ""
        : fmt::format("MultiInputDialogInput_{}_{}", impl_->id, options.inputFields.front().key);

    impl_->dialog->open({
        .headerText = options.headerText,
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Cancel,
        .initialFocus = initialFocus,
        .onClose =
            [this](std::optional<Snc::Dialog::Button> button)
        {
            if (!impl_->onConfirm)
                return;

            if (button && *button == Snc::Dialog::Button::Ok)
                impl_->onConfirm(impl_->values);
            else
                impl_->onConfirm(std::nullopt);

            impl_->values.clear();
        },
        .modal = true,
        .mayCloseWithoutButton = false,
    });
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(MultiInputDialog);

Nui::ElementRenderer MultiInputDialog::operator()()
{
    return (*impl_->dialog)();
}