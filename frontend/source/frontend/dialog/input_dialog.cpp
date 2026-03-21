#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <log/log.hpp>

#include <script-nui-components/button.hpp>

#include <script-nui-components/dialog.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

struct InputDialog::Implementation
{
    std::string id;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    Nui::Observed<std::string> inputText;
    std::function<void(std::optional<std::string> const&)> onConfirm;
    std::string headerText{};
    Nui::Observed<bool> isPassword{false};
    Nui::Observed<std::string> whatFor{};

    Implementation(std::string id)
        : id{std::move(id)}
        , dialog{}
        , onConfirm{}
    {}
};

InputDialog::InputDialog(std::string id)
    : impl_{std::make_unique<Implementation>(id)}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, dialogBody());
}

Nui::ElementRenderer InputDialog::dialogBody()
{
    namespace Snc = ScriptNuiComponents;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return section{class_ = "input-dialog"}(
        span{}(
            observe(impl_->whatFor),
            [this]() -> Nui::ElementRenderer {
                if (impl_->whatFor.empty())
                    return Nui::nil();
                return Nui::Elements::text{fmt::format("{}:", impl_->whatFor.value())}();
            }
        ),
        Snc::textInput({
            .value = impl_->inputText,
            .attributes = {
                id = fmt::format("{}-input", impl_->id),
                type = observe(impl_->isPassword).generate([this](){
                    return impl_->isPassword.value() ? "Password" : "Text";
                }),
                "keyup"_event = [this](Nui::WebApi::KeyboardEvent event){
                    dialogButtonContainerKeydown(event);
                    if (event.key() == "Enter") {
                        impl_->dialog->close();
                        impl_->onConfirm(impl_->inputText.value());
                    }
                }
            }
        })
    );
    // clang-format on
}

void InputDialog::open(OpenOptions const& options)
{
    namespace Snc = ScriptNuiComponents;

    impl_->onConfirm = options.onConfirm;
    impl_->headerText = options.headerText;
    impl_->isPassword = options.isPassword;
    impl_->whatFor = options.whatFor;
    impl_->inputText.clear();
    Nui::globalEventContext.executeActiveEventsImmediately();

    impl_->dialog->open({
        .headerText = impl_->headerText,
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Cancel,
        .initialFocus = fmt::format("{}-input", impl_->id),
        .onClose =
            [this](std::optional<Snc::Dialog::Button> button)
        {
            if (!impl_->onConfirm)
                return;

            if (button && *button == Snc::Dialog::Button::Ok)
                impl_->onConfirm(impl_->inputText.value());
            else
                impl_->onConfirm(std::nullopt);
        },
        .modal = true,
        .mayCloseWithoutButton = false,
    });
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(InputDialog);

Nui::ElementRenderer InputDialog::operator()()
{
    return (*impl_->dialog)();
}
