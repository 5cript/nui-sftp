#pragma once

#include <ftp/client/tls.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace FtpConnect
{
    /**
     * @brief A certificate that neither the authorities nor the pin of the session vouched for, what a trust prompt
     *        asks about.
     */
    struct TrustQuestion
    {
        std::string host{};
        std::uint16_t port{};
        FileTransferProtocol::CertificateInfo certificate{};
        /** @brief The session pins another key: the key of the server changed since it was trusted. */
        bool pinnedKeyDiffers{false};
    };

    /**
     * @brief @p question as the trust prompt of the frontend reads it: host, port, subject, issuer, notBefore and
     *        notAfter in UTC, subjectAlternativeNames, publicKeySha256 and certificateSha256 as formatDigest() writes
     *        them, verifyFailure and pinnedKeyDiffers.
     */
    nlohmann::json toJson(TrustQuestion const& question);

    /**
     * @brief Asks whether to trust a certificate.
     *
     * @return The SHA-256 of the public key the user trusts, nothing when the prompt was refused or cancelled.
     */
    using TrustSource = std::function<std::optional<FileTransferProtocol::Sha256Digest>(TrustQuestion const& question)>;

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
         * @param onAnswer Called once, from any thread, with the SHA-256 of the public key the user trusts as
         *        formatDigest() writes it, nothing when the user refused.
         */
        virtual void
        askTrust(TrustQuestion const& question, std::function<void(std::optional<std::string>)> onAnswer) = 0;
    };
}
