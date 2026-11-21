#pragma once

#include <backend/ftp/certificate_trust.hpp>

#include <nui/rpc.hpp>

#include <functional>
#include <mutex>
#include <optional>
#include <string>

/**
 * @brief Asks the frontend whether to trust a certificate: "CertificateTrustPrompter::prompt" with toJson() of the
 *        question, answered by "CertificateTrustPrompter::promptDone" with the trusted public key SHA-256, or an empty
 *        string for none.
 */
class CertificateTrustPrompter : public FtpConnect::TrustProvider
{
  public:
    explicit CertificateTrustPrompter(Nui::RpcHub& rpcHub);
    ~CertificateTrustPrompter() override = default;
    CertificateTrustPrompter(CertificateTrustPrompter const&) = delete;
    CertificateTrustPrompter& operator=(CertificateTrustPrompter const&) = delete;
    CertificateTrustPrompter(CertificateTrustPrompter&&) = delete;
    CertificateTrustPrompter& operator=(CertificateTrustPrompter&&) = delete;

    void registerRpc();

    /**
     * @brief Shows the prompt. A prompt still waiting is answered with nothing first.
     */
    void askTrust(
        FtpConnect::TrustQuestion const& question,
        std::function<void(std::optional<std::string>)> onAnswer
    ) override;

  private:
    /**
     * @brief The answer of the waiting prompt, which it takes.
     */
    std::function<void(std::optional<std::string>)> takeOnAnswer();

  private:
    Nui::RpcHub* rpcHub_;
    std::mutex onAnswerMutex_{};
    std::function<void(std::optional<std::string>)> onAnswer_{};
};
