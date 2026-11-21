#pragma once

#include <ftp/client/tls.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace FileTransferProtocol
{
    enum class PlatformVerdict
    {
        /** @brief The system trusts the chain for the server name. */
        Trusted,
        /** @brief The system does not trust it, could not be asked or the check was stopped. */
        Refused,
        /** @brief The platform has no check of its own beyond the authorities OpenSSL loaded. */
        Unavailable,
    };

    /**
     * @brief Asks the system whether it trusts @p chain, the certificates the server sent with its own first, for the
     *        TLS server @p serverName. macOS: SecTrust with the SSL policy, which sees the Keychain. Windows: the chain
     *        engine of the current user with the SSL policy, which sees the certificate stores and the disallowed
     *        certificates, and asks for no revocation check. Both may fetch certificates over the network, on Windows
     *        also authorities the system has not needed before. Blocks until the system answered or @p stop is
     *        requested. Unavailable elsewhere.
     *
     * @param exclusiveAnchors The only anchors to trust, in place of the ones of the system. For the tests.
     */
    PlatformVerdict platformVerdictOf(
        std::vector<DerCertificate> const& chain,
        std::string const& serverName,
        std::stop_token const& stop,
        std::optional<std::vector<DerCertificate>> const& exclusiveAnchors = std::nullopt
    );
}
