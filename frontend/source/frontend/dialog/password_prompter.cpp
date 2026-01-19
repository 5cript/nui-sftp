#include <frontend/dialog/password_prompter.hpp>
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

struct PasswordPrompter::Implementation
{
    InputDialog dialog{"PasswordPrompter"};
};

PasswordPrompter::PasswordPrompter()
    : impl_{std::make_unique<Implementation>()}
{
    Nui::RpcClient::registerFunction(
        "PasswordPrompter::prompt",
        [this](std::string const& whatFor, std::string const& prompt)
        {
            Log::info("Opening password prompt for '{}'", whatFor);
            impl_->dialog.open({
                .whatFor = whatFor,
                .prompt = prompt,
                .headerText = "Enter Password",
                .isPassword = true,
                .onConfirm = [](std::optional<std::string> const& password)
                {
                    Nui::RpcClient::call("PasswordPrompter::promptDone", password.value_or(""));
                },
            });
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(PasswordPrompter);

Nui::ElementRenderer PasswordPrompter::dialog()
{
    return impl_->dialog();
}
