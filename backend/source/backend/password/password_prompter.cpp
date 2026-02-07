#include <backend/password/password_prompter.hpp>

PasswordPrompter::PasswordPrompter(Nui::RpcHub& rpcHub)
    : rpcHub_(&rpcHub)
{}

void PasswordPrompter::registerRpc()
{
    rpcHub_->registerFunction(
        "PasswordPrompter::promptDone",
        [this](std::string const& result)
        {
            if (ready_)
            {
                if (result.empty())
                    ready_(std::nullopt);
                else
                    ready_(result);
            }
            ready_ = {};
        }
    );
}

void PasswordPrompter::getPassword(
    std::string const& whatFor,
    std::string const& prompt,
    std::function<void(std::optional<std::string>)> const& onPasswordReady
)
{
    ready_ = onPasswordReady;
    rpcHub_->call("PasswordPrompter::prompt", whatFor, prompt);
}