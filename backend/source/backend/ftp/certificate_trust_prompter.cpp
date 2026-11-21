#include <backend/ftp/certificate_trust_prompter.hpp>

#include <utility>

CertificateTrustPrompter::CertificateTrustPrompter(Nui::RpcHub& rpcHub)
    : rpcHub_{&rpcHub}
{}

void CertificateTrustPrompter::registerRpc()
{
    rpcHub_->registerFunction(
        "CertificateTrustPrompter::promptDone",
        [this](std::string const& trustedKey)
        {
            const auto onAnswer = takeOnAnswer();
            if (!onAnswer)
                return;
            if (trustedKey.empty())
                return onAnswer(std::nullopt);
            onAnswer(trustedKey);
        }
    );
}

void CertificateTrustPrompter::askTrust(
    FtpConnect::TrustQuestion const& question,
    std::function<void(std::optional<std::string>)> onAnswer
)
{
    auto waiting = [this, &onAnswer]()
    {
        std::scoped_lock lock{onAnswerMutex_};
        return std::exchange(onAnswer_, std::move(onAnswer));
    }();
    if (waiting)
        waiting(std::nullopt);
    rpcHub_->call("CertificateTrustPrompter::prompt", FtpConnect::toJson(question));
}

std::function<void(std::optional<std::string>)> CertificateTrustPrompter::takeOnAnswer()
{
    std::scoped_lock lock{onAnswerMutex_};
    return std::exchange(onAnswer_, {});
}
