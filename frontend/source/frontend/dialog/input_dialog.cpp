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
    Nui::Observed<bool> showPassword{false};
    Nui::Observed<std::string> whatFor{};
    bool confirmOnClose{false};

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
        span{class_ = "input-dialog-label"}(
            observe(impl_->whatFor),
            [this]() -> Nui::ElementRenderer {
                if (impl_->whatFor.empty())
                    return Nui::nil();
                return Nui::Elements::text{fmt::format("{}:", impl_->whatFor.value())}();
            }
        ),
        div{class_ = "input-dialog-input-container"}(
            Snc::textInput({
                .value = impl_->inputText,
                .attributes = {
                    id = fmt::format("{}-input", impl_->id),
                    type = observe(impl_->isPassword, impl_->showPassword).generate([this](){
                        if (!impl_->isPassword.value()) return std::string{"Text"};
                        return impl_->showPassword.value() ? std::string{"Text"} : std::string{"Password"};
                    }),
                    "input"_event = [this](Nui::WebApi::Event event){
                        impl_->inputText = event.target()["value"].as<std::string>();
                    },
                    "keydown"_event = [this](Nui::WebApi::KeyboardEvent event){
                        dialogButtonContainerKeydown(event);
                        if (event.key() == "Enter") {
                            event.preventDefault();
                            impl_->inputText = event.target()["value"].as<std::string>();
                            impl_->confirmOnClose = true;
                            impl_->dialog->close();
                        }
                    }
                },
                .dontUpdateValue = true,
            }),
            div{class_ = "input-dialog-toggle-btn-wrapper"}(
                observe(impl_->isPassword, impl_->showPassword),
                [this]() -> Nui::ElementRenderer {
                    if (!impl_->isPassword.value())
                        return Nui::nil();
                    return ScriptNuiComponents::button({
                        .text = impl_->showPassword.value() ? "Hide" : "Show",
                        .attributes = {
                            class_ = "input-dialog-toggle-btn",
                            onClick = [this](){
                                impl_->showPassword = !impl_->showPassword.value();
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                        }
                    });
                }
            )
        )
    );
    // clang-format on
}

void InputDialog::open(OpenOptions const& options)
{
    namespace Snc = ScriptNuiComponents;

    impl_->onConfirm = options.onConfirm;
    impl_->headerText = options.headerText;
    impl_->isPassword = options.isPassword;
    impl_->showPassword = false;
    impl_->confirmOnClose = false;
    impl_->whatFor = options.whatFor;
    impl_->inputText = options.initialValue;
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

            if ((button && *button == Snc::Dialog::Button::Ok) || impl_->confirmOnClose)
            {
                impl_->confirmOnClose = false;
                impl_->onConfirm(impl_->inputText.value());
                // Overscramble the memory, then clear:
                for (auto i = std::size_t{}; i != impl_->inputText.value().size(); ++i)
                    impl_->inputText.value()[i] = 'A';
                impl_->inputText = "";
            }
            else
            {
                impl_->confirmOnClose = false;
                impl_->onConfirm(std::nullopt);
            }
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
