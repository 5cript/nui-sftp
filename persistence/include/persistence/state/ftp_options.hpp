#pragma once

#include <persistence/state_core.hpp>

#include <optional>

namespace Persistence
{
    /**
     * @brief How a data connection is asked for: EPSV with PASV after a refusal, EPSV only, or PASV only.
     */
    enum class FtpExtendedPassive
    {
        Automatic,
        Always,
        Never
    };
    BOOST_DESCRIBE_ENUM(FtpExtendedPassive, Automatic, Always, Never)

    enum class FtpTlsVersion
    {
        Tls12,
        Tls13
    };
    BOOST_DESCRIBE_ENUM(FtpTlsVersion, Tls12, Tls13)

    /**
     * @brief The connection options of FTP sessions, shared by reference like the SSH options. Unset means the
     *        default of the FTP client.
     */
    struct FtpOptions : public DefaultMissingMember
    {
        /** @brief The TCP connect and the greeting of a control connection, and the connect of a data connection. */
        std::optional<int> connectTimeoutSeconds{std::nullopt};
        /** @brief From writing a command to its first reply. */
        std::optional<int> commandTimeoutSeconds{std::nullopt};
        /** @brief The longest wait for data during a transfer. */
        std::optional<int> dataInactivityTimeoutSeconds{std::nullopt};
        /** @brief From the end of the data of a transfer to the server's final reply. */
        std::optional<int> transferReplyTimeoutSeconds{std::nullopt};
        /** @brief Control connections of a session, one for browsing and the others for transfers. At least 2. */
        std::optional<int> maxConnections{std::nullopt};
        std::optional<FtpExtendedPassive> extendedPassive{std::nullopt};
        /** @brief Connects to the address a PASV reply names instead of the server's address. */
        std::optional<bool> trustPassiveAddress{std::nullopt};
        /** @brief Sends NOOP on an idle session, so the server's idle timeout does not end it. */
        std::optional<bool> keepIdleAlive{std::nullopt};
        std::optional<int> idleNoopIntervalSeconds{std::nullopt};
        std::optional<int> keepaliveIdleSeconds{std::nullopt};
        std::optional<int> keepaliveIntervalSeconds{std::nullopt};
        std::optional<int> keepaliveCount{std::nullopt};
        std::optional<FtpTlsVersion> minimumTlsVersion{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        FtpOptions,
        (),
        (connectTimeoutSeconds,
            commandTimeoutSeconds,
            dataInactivityTimeoutSeconds,
            transferReplyTimeoutSeconds,
            maxConnections,
            extendedPassive,
            trustPassiveAddress,
            keepIdleAlive,
            idleNoopIntervalSeconds,
            keepaliveIdleSeconds,
            keepaliveIntervalSeconds,
            keepaliveCount,
            minimumTlsVersion)
    )
}
