#pragma once

#include <backend/ftp/certificate_trust.hpp>
#include <backend/ssh/host_key_provider.hpp>

#include <nui/rpc.hpp>

#include <functional>

/**
 * @brief Host key trust towards the frontend, which gets what it shows and gives a decision, never a key:
 *        "HostKeyPrompter::prompt" asks with problem ("unknown", "changed", "otherType" or
 *        "changedSinceTrusted"), jumpHost, host, port, keyType, fingerprint, knownHostsFile, trustedKey,
 *        removalCommand and a promptId, answered by "HostKeyPrompter::promptDone" with that promptId and "always" or
 *        "refuse", anything else refuses. A changed key is only shown as a warning, its answer changes nothing.
 */
class HostKeyPrompter : public HostKeyProvider
{
  public:
    explicit HostKeyPrompter(Nui::RpcHub& rpcHub);
    ~HostKeyPrompter() override = default;
    HostKeyPrompter(HostKeyPrompter const&) = delete;
    HostKeyPrompter& operator=(HostKeyPrompter const&) = delete;
    HostKeyPrompter(HostKeyPrompter&&) = delete;
    HostKeyPrompter& operator=(HostKeyPrompter&&) = delete;

    void registerRpc();

    /**
     * @brief What a decision of the frontend means for a host key. Trust without saving is nothing the prompt
     *        offers, and refuses.
     */
    static SecureShell::HostKeyAnswer answerOf(FtpConnect::TrustAnswer decision);

    /**
     * @brief Shows the prompt. A prompt still waiting is refused first.
     */
    void askAboutHostKey(
        SecureShell::HostKeyQuestion const& question,
        std::function<void(SecureShell::HostKeyAnswer)> onAnswer
    ) override;

  private:
    Nui::RpcHub* rpcHub_;
    FtpConnect::PendingTrustPrompt pending_{};
};
