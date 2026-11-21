#pragma once

#include <ftp/client/platform_trust.hpp>
#include <ftp/client/tls.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace FtpConnect
{
    /**
     * @brief A certificate the backend refused: the authorities did not vouch for it, or the server is trusted with
     *        another key. What a trust prompt asks about.
     */
    struct TrustQuestion
    {
        std::string host{};
        std::uint16_t port{};
        FileTransferProtocol::CertificateInfo certificate{};
        /** @brief Another key is trusted for the server: its key changed since it was trusted. */
        bool pinnedKeyDiffers{false};
    };

    /**
     * @brief What the user decided about a certificate. It names no key: the backend trusts the key of the certificate
     *        it asked about.
     */
    enum class TrustAnswer
    {
        /** @brief Also a prompt that was cancelled or could not be shown. */
        Refused,
        /** @brief Until the application ends. */
        Once,
        /** @brief Saved, for later runs of the application too. */
        Always,
    };

    /**
     * @brief @p question as the trust prompt of the frontend shows it: host, port, subject, issuer, notBefore and
     *        notAfter in UTC, subjectAlternativeNames, publicKeySha256 and certificateSha256 as formatDigest() writes
     *        them, pinnedKeyDiffers and verifyFailures, each with its kind (nameMismatch, expired, notYetValid,
     *        selfSigned, unknownAuthority, other) and its reason in the words of OpenSSL.
     */
    nlohmann::json toJson(TrustQuestion const& question);

    /**
     * @brief Asks whether to trust a certificate.
     */
    using TrustSource = std::function<TrustAnswer(TrustQuestion const& question)>;

    /**
     * @brief Asks the system whether it trusts the chain a server sent for its name, see
     *        FileTransferProtocol::platformVerdictOf().
     */
    using PlatformTrust = std::function<FileTransferProtocol::PlatformVerdict(
        std::vector<FileTransferProtocol::DerCertificate> const& chain,
        std::string const& serverName,
        std::stop_token const& stop
    )>;

    /**
     * @brief Asks the user whether to trust a certificate, see SessionManager::setTrustProvider().
     */
    class TrustProvider
    {
      public:
        TrustProvider() = default;
        virtual ~TrustProvider() = default;
        TrustProvider(TrustProvider const&) = delete;
        TrustProvider& operator=(TrustProvider const&) = delete;
        TrustProvider(TrustProvider&&) = delete;
        TrustProvider& operator=(TrustProvider&&) = delete;

        /**
         * @param onAnswer Called once, from any thread, with what the user decided.
         */
        virtual void askTrust(TrustQuestion const& question, std::function<void(TrustAnswer)> onAnswer) = 0;
    };

    /**
     * @brief The one trust prompt that waits for the answer of the frontend. An answer names the prompt by its id and
     *        the decision as "once", "always" or "refuse", nothing of the certificate: any other decision refuses, and
     *        an answer to another prompt than the waiting one is dropped. Thread-safe.
     */
    class PendingTrustPrompt
    {
      public:
        /**
         * @brief Makes @p onAnswer the waiting prompt. A prompt still waiting is refused first.
         *
         * @return The id an answer has to name.
         */
        int begin(std::function<void(TrustAnswer)> onAnswer);

        /**
         * @brief Answers the waiting prompt when @p promptId is its id.
         *
         * @return Whether the prompt was answered.
         */
        bool answer(int promptId, std::string_view decision);

      private:
        std::mutex mutex_{};
        int waitingId_{0};
        std::function<void(TrustAnswer)> onAnswer_{};
    };
}
