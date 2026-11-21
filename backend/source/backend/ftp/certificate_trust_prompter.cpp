#include <backend/ftp/certificate_trust_prompter.hpp>
#include <backend/rpc_helper.hpp>
#include <log/log.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace
{
    std::optional<std::uint16_t> portOf(int port)
    {
        if (port < 1 || port > std::numeric_limits<std::uint16_t>::max())
            return std::nullopt;
        return static_cast<std::uint16_t>(port);
    }
}

CertificateTrustPrompter::CertificateTrustPrompter(
    Nui::RpcHub& rpcHub,
    std::shared_ptr<FtpConnect::CertificateTrustStore> store
)
    : rpcHub_{&rpcHub}
    , store_{std::move(store)}
{}

void CertificateTrustPrompter::registerRpc()
{
    rpcHub_->registerFunction(
        "CertificateTrustPrompter::promptDone",
        [this](int promptId, std::string const& decision)
        {
            pending_.answer(promptId, decision);
        }
    );
    rpcHub_->registerFunction(
        "CertificateTrust::trustedKey",
        [this](std::string const& responseId, std::string const& host, int port)
        {
            const auto trusted = portOf(port).and_then(
                [this, &host](std::uint16_t validPort)
                {
                    return store_->trustedKeyOf(host, validPort);
                }
            );
            RpcHelper::callRemoteSafely(
                *rpcHub_,
                responseId,
                nlohmann::json{
                    {"publicKeySha256",
                        trusted ? FileTransferProtocol::formatDigest(trusted->publicKey) : std::string{}},
                    {"lasting", trusted && trusted->lasting},
                }
            );
        }
    );
    rpcHub_->registerFunction(
        "CertificateTrust::forget",
        [this](std::string const& responseId, std::string const& host, int port)
        {
            const auto validPort = portOf(port);
            if (!validPort)
                return RpcHelper::callRemoteSafely(*rpcHub_, responseId, nlohmann::json{{"error", "No valid port."}});
            Log::info("Forgetting the trusted key of the FTP server {}:{}.", host, *validPort);
            const auto forgotten = store_->forget(host, *validPort);
            if (!forgotten)
            {
                Log::error("The trusted keys of FTP servers could not be saved: {}", forgotten.error());
                return RpcHelper::callRemoteSafely(*rpcHub_, responseId, nlohmann::json{{"error", forgotten.error()}});
            }
            RpcHelper::callRemoteSafely(*rpcHub_, responseId, nlohmann::json::object());
        }
    );
}

void CertificateTrustPrompter::askTrust(
    FtpConnect::TrustQuestion const& question,
    std::function<void(FtpConnect::TrustAnswer)> onAnswer
)
{
    auto prompt = FtpConnect::toJson(question);
    prompt["promptId"] = pending_.begin(std::move(onAnswer));
    rpcHub_->call("CertificateTrustPrompter::prompt", prompt);
}
