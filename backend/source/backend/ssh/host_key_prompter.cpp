#include <backend/ssh/host_key_prompter.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace
{
    std::string_view nameOf(SecureShell::HostKeyProblem problem)
    {
        switch (problem)
        {
            case SecureShell::HostKeyProblem::Changed:
                return "changed";
            case SecureShell::HostKeyProblem::OtherType:
                return "otherType";
            case SecureShell::HostKeyProblem::ChangedSinceTrusted:
                return "changedSinceTrusted";
            case SecureShell::HostKeyProblem::Unknown:
                break;
        }
        return "unknown";
    }
}

HostKeyPrompter::HostKeyPrompter(Nui::RpcHub& rpcHub)
    : rpcHub_{&rpcHub}
{}

SecureShell::HostKeyAnswer HostKeyPrompter::answerOf(FtpConnect::TrustAnswer decision)
{
    switch (decision)
    {
        case FtpConnect::TrustAnswer::Always:
            return SecureShell::HostKeyAnswer::Always;
        case FtpConnect::TrustAnswer::Once:
        case FtpConnect::TrustAnswer::Refused:
            break;
    }
    return SecureShell::HostKeyAnswer::Refused;
}

void HostKeyPrompter::registerRpc()
{
    rpcHub_->registerFunction(
        "HostKeyPrompter::promptDone",
        [this](int promptId, std::string const& decision)
        {
            pending_.answer(promptId, decision);
        }
    );
}

void HostKeyPrompter::askAboutHostKey(
    SecureShell::HostKeyQuestion const& question,
    std::function<void(SecureShell::HostKeyAnswer)> onAnswer
)
{
    const auto promptId = pending_.begin(
        [onAnswer = std::move(onAnswer)](FtpConnect::TrustAnswer answer)
        {
            onAnswer(answerOf(answer));
        }
    );
    rpcHub_->call(
        "HostKeyPrompter::prompt",
        nlohmann::json{
            {"promptId", promptId},
            {"problem", nameOf(question.problem)},
            {"jumpHost", question.jumpHost},
            {"host", question.host},
            {"port", question.port},
            {"keyType", question.keyType},
            {"fingerprint", question.fingerprint},
            {"knownHostsFile", question.knownHostsFile},
            {"trustedKey", question.trustedKey},
            {"removalCommand", question.removalCommand},
        }
    );
}
