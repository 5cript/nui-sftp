#pragma once

#include <remote_io/remote_error.hpp>

#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/ssl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol
{
    enum class TlsMode
    {
        /** @brief Plain FTP, nothing is protected. */
        None,
        /** @brief AUTH TLS on the plain control connection (RFC 4217). */
        Explicit,
        /** @brief TLS from the first byte, usually on port 990. */
        Implicit,
    };

    enum class TlsVersion
    {
        Tls12,
        Tls13,
    };

    /**
     * @brief A SHA-256 digest of a certificate or of its public key.
     */
    using Sha256Digest = std::array<std::uint8_t, 32>;

    /**
     * @brief A certificate in DER.
     */
    using DerCertificate = std::vector<std::uint8_t>;

    /**
     * @brief @p digest as upper case hex pairs separated by colons, the way certificate viewers show fingerprints.
     */
    std::string formatDigest(Sha256Digest const& digest);

    /**
     * @brief Reads a digest of 64 hex digits in any case, plain or as 32 pairs with a colon between each two.
     */
    std::optional<Sha256Digest> parseDigest(std::string_view text);

    enum class VerifyFailureKind
    {
        /** @brief The certificate names other servers. */
        NameMismatch,
        Expired,
        NotYetValid,
        /** @brief Signed by itself, or its chain ends in such a certificate nobody trusts. */
        SelfSigned,
        /** @brief Signed by an authority that is not trusted or not known. */
        UnknownAuthority,
        Other,
    };

    /**
     * @brief One reason the check of a certificate against the trusted authorities failed.
     */
    struct VerifyFailure
    {
        VerifyFailureKind kind{VerifyFailureKind::Other};
        /** @brief The reason in the words of OpenSSL. */
        std::string reason{};

        bool operator==(VerifyFailure const&) const = default;
    };

    /**
     * @brief What a server certificate says, for the trust prompt and for the pin.
     */
    struct CertificateInfo
    {
        /** @brief The subject in RFC 2253 form. */
        std::string subject{};
        /** @brief The issuer in RFC 2253 form. */
        std::string issuer{};
        std::chrono::sys_seconds notBefore{};
        std::chrono::sys_seconds notAfter{};
        /** @brief DNS names and IP addresses, as "DNS:localhost" and "IP:127.0.0.1". */
        std::vector<std::string> subjectAlternativeNames{};
        /** @brief Of the whole certificate in DER. */
        Sha256Digest certificateSha256{};
        /** @brief Of the SubjectPublicKeyInfo in DER, what a pin names. */
        Sha256Digest publicKeySha256{};
        /** @brief Every reason the trusted authorities did not vouch for it, as found, empty when they did. */
        std::vector<VerifyFailure> verifyFailures{};
        /** @brief The certificates the server sent, its own first. Only for a refused certificate. */
        std::vector<DerCertificate> presentedChain{};
    };

    /**
     * @brief The reasons of @p failures in one line, empty without one.
     */
    std::string reasonsOf(std::vector<VerifyFailure> const& failures);

    /**
     * @brief The facts of @p certificate.
     *
     * @return Nothing when OpenSSL cannot read one of them.
     */
    std::optional<CertificateInfo> describeCertificate(X509* certificate);

    enum class TrustSource
    {
        /** @brief A trusted authority signed it and it names the server. A pin, when there is one, names its key. */
        Authorities,
        /** @brief The authorities did not vouch for it, its public key matched the pin. */
        Pin,
    };

    struct TrustedCertificate
    {
        CertificateInfo certificate{};
        TrustSource source{TrustSource::Authorities};
    };

    struct TlsContextOptions
    {
        /** @brief Trusts the authorities of the system, see addSystemAuthorities(). */
        bool systemAuthorities{true};
        /** @brief Certificates in PEM of more authorities to trust. */
        std::vector<std::string> authorities{};
        TlsVersion minimumVersion{TlsVersion::Tls12};
    };

    /**
     * @brief The client settings all TLS connections of a session share: the trusted authorities, the minimum version,
     *        ALPN "ftp". OpenSSL's client session cache is off, data connections resume a copy of the control
     *        connection's session instead. Renegotiation is refused: a TLS 1.2 server that asks for it gets a
     *        no_renegotiation alert, which OpenSSL servers answer by ending the connection.
     */
    class TlsContext
    {
      public:
        struct Private
        {};

        /**
         * @return A Failure naming the setting OpenSSL refused.
         */
        static std::expected<std::shared_ptr<TlsContext>, RemoteIo::RemoteError> create(TlsContextOptions options);

        TlsContext(Private, SSL_CTX* adopted);
        ~TlsContext();
        TlsContext(TlsContext const&) = delete;
        TlsContext& operator=(TlsContext const&) = delete;
        TlsContext(TlsContext&&) = delete;
        TlsContext& operator=(TlsContext&&) = delete;

        boost::asio::ssl::context& context();

      private:
        boost::asio::ssl::context context_;
    };

    /**
     * @brief How one connection checks the server's certificate.
     */
    struct TlsConnectionSettings
    {
        std::shared_ptr<TlsContext> context{};
        /** @brief The name the certificate has to carry, a DNS name (also sent as SNI) or an IP address. */
        std::string serverName{};
        /**
         * @brief The one public key the server may present: a certificate with it is trusted also when the
         *        authorities do not vouch for it, and no other certificate is, whoever vouches for it.
         */
        std::optional<Sha256Digest> pinnedPublicKey{};
    };

    struct SslDeleter
    {
        void operator()(SSL* handle) const noexcept;
    };
    using SslHandle = std::unique_ptr<SSL, SslDeleter>;

    struct SslSessionDeleter
    {
        void operator()(SSL_SESSION* session) const noexcept;
    };
    using SslSessionHandle = std::unique_ptr<SSL_SESSION, SslSessionDeleter>;

    /**
     * @brief A handle for one client connection, ready for the handshake: the name check without partial wildcards,
     *        SNI for a DNS name, and a certificate check that never blocks. The check keeps the certificate and every
     *        verify failure. Without a pin it accepts what the authorities vouch for, with a pin only a certificate
     *        whose public key matches it.
     *
     * @return A Failure naming the setting OpenSSL refused.
     */
    std::expected<SslHandle, RemoteIo::RemoteError> makeTlsHandle(TlsConnectionSettings const& settings);

    /**
     * @brief Who vouched for the certificate of a completed handshake on @p handle.
     *
     * @return NoSecureConnection when the handshake resumed a session, which checks no certificate, or left no
     *         certificate behind, which the certificate check does not allow.
     */
    std::expected<TrustedCertificate, RemoteIo::RemoteError> trustOf(SSL* handle);

    /**
     * @brief The SHA-256 of the certificate the peer presented on @p handle, of the whole certificate in DER.
     *
     * @return Nothing without a certificate, or when OpenSSL cannot hash it.
     */
    std::optional<Sha256Digest> peerCertificateDigest(SSL* handle);

    /**
     * @brief Why a handshake on @p handle failed: TrustRequired when the certificate check refused the certificate,
     *        otherwise NoSecureConnection that says whether it timed out (@p timedOut), the stream ended or what
     *        OpenSSL reported.
     */
    RemoteIo::RemoteError handshakeError(SSL* handle, boost::system::error_code error, bool timedOut);

    /**
     * @brief @p error as the reason a connection got no TLS: NoSecureConnection with its message and code. A
     *        TrustRequired, which the trust prompt answers, and a cancel keep their kind.
     */
    RemoteIo::RemoteError asNoSecureConnection(RemoteIo::RemoteError error);

    /**
     * @brief The certificate the certificate check refused on @p handle. Without verify failures it was refused for
     *        its public key alone, which is not the pinned one.
     */
    std::optional<CertificateInfo> refusedCertificate(SSL* handle);

    /**
     * @brief A copy of the session of @p handle that a data connection can resume, each data connection needs its
     *        own: OpenSSL makes a session unusable when a connection that used it ends without close_notify.
     *
     * @return Nothing while the session cannot be resumed, before a TLS 1.3 ticket was read for one.
     */
    std::optional<SslSessionHandle> resumableSession(SSL* handle);
}
