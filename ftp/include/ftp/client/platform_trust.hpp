#pragma once

#include <ftp/client/tls.hpp>

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
     * @brief Anchors a check trusts besides the system's or in their place, for the tests.
     */
    struct PlatformAnchors
    {
        std::vector<DerCertificate> certificates{};
        /** @brief Trusts the certificates alone, not the anchors of the system. */
        bool only{false};
    };

    /**
     * @brief Asks the system whether it trusts @p chain, the certificates the server sent with its own first, for
     *        @p serverName: on macOS SecTrust with the SSL policy, which sees the Keychain and may fetch missing
     *        intermediates. Blocks until the system answered or @p stop is requested. Unavailable elsewhere.
     */
    PlatformVerdict platformVerdictOf(
        std::vector<DerCertificate> const& chain,
        std::string const& serverName,
        std::stop_token const& stop,
        PlatformAnchors const& anchors = {}
    );
}
