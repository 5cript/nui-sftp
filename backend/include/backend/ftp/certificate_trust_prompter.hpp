#pragma once

#include <backend/ftp/certificate_trust.hpp>
#include <backend/ftp/certificate_trust_store.hpp>

#include <nui/rpc.hpp>

#include <functional>
#include <memory>

/**
 * @brief Certificate trust towards the frontend, which gets what it shows and gives decisions, never a key or a
 *        certificate:
 *        - "CertificateTrustPrompter::prompt" asks with toJson() of the question and a promptId, answered by
 *          "CertificateTrustPrompter::promptDone" with that promptId and "once", "always" or "refuse".
 *        - "CertificateTrust::trustedKey" of a host and port replies with publicKeySha256, empty without a trusted
 *          key, and whether it is lasting.
 *        - "CertificateTrust::forget" of a host and port forgets its key and replies with an error when there is one.
 */
class CertificateTrustPrompter : public FtpConnect::TrustProvider
{
  public:
    CertificateTrustPrompter(Nui::RpcHub& rpcHub, std::shared_ptr<FtpConnect::CertificateTrustStore> store);
    ~CertificateTrustPrompter() override = default;
    CertificateTrustPrompter(CertificateTrustPrompter const&) = delete;
    CertificateTrustPrompter& operator=(CertificateTrustPrompter const&) = delete;
    CertificateTrustPrompter(CertificateTrustPrompter&&) = delete;
    CertificateTrustPrompter& operator=(CertificateTrustPrompter&&) = delete;

    void registerRpc();

    /**
     * @brief Shows the prompt. A prompt still waiting is refused first.
     */
    void
    askTrust(FtpConnect::TrustQuestion const& question, std::function<void(FtpConnect::TrustAnswer)> onAnswer) override;

  private:
    Nui::RpcHub* rpcHub_;
    std::shared_ptr<FtpConnect::CertificateTrustStore> store_;
    FtpConnect::PendingTrustPrompt pending_{};
};
