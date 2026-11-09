#include <ftp/client/tls.hpp>
#include <ftp/protocol/protocol_error.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ssl/error.hpp>
#include <fmt/format.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ctime>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        using RemoteIo::RemoteErrorKind;

        template <auto release>
        struct Release
        {
            template <typename Resource>
            void operator()(Resource* resource) const noexcept
            {
                release(resource);
            }
        };
        using ContextHandle = std::unique_ptr<SSL_CTX, Release<SSL_CTX_free>>;
        using CertificateHandle = std::unique_ptr<X509, Release<X509_free>>;
        using MemoryBio = std::unique_ptr<BIO, Release<BIO_free_all>>;
        using GeneralNames = std::unique_ptr<GENERAL_NAMES, Release<GENERAL_NAMES_free>>;

        constexpr std::array<unsigned char, 4> applicationProtocols{3u, 'f', 't', 'p'};

        RemoteIo::RemoteError errorOf(RemoteErrorKind kind, std::string message)
        {
            return RemoteIo::RemoteError{
                .kind = kind,
                .protocol = RemoteIo::Protocol::Ftp,
                .message = std::move(message),
            };
        }

        /**
         * @brief The oldest error OpenSSL queued, the queue is empty afterwards.
         */
        std::string openSslReason()
        {
            const auto code = ERR_get_error();
            ERR_clear_error();
            if (code == 0u)
                return "no reason given";
            std::array<char, 256> reason{};
            ERR_error_string_n(code, reason.data(), reason.size());
            return reason.data();
        }

        RemoteIo::RemoteError refused(std::string_view what)
        {
            return errorOf(RemoteErrorKind::Failure, fmt::format("OpenSSL refused {}: {}", what, openSslReason()));
        }

        /**
         * @brief What the verify callback found out during one handshake, freed with the SSL that holds it.
         */
        struct Verification
        {
            std::optional<Sha256Digest> pin{};
            CertificateHandle leaf{};
            int firstFailure{X509_V_OK};
            bool trustedByPin{false};
            bool refused{false};
        };

        void freeVerification(void*, void* pointer, CRYPTO_EX_DATA*, int, long, void*)
        {
            delete static_cast<Verification*>(pointer);
        }

        int verificationIndex()
        {
            static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, &freeVerification);
            return index;
        }

        Verification* verificationOf(SSL* handle)
        {
            const auto index = verificationIndex();
            if (handle == nullptr || index < 0)
                return nullptr;
            return static_cast<Verification*>(SSL_get_ex_data(handle, index));
        }

        std::optional<Sha256Digest> sha256Of(unsigned char const* bytes, std::size_t size)
        {
            Sha256Digest digest{};
            unsigned int length = 0u;
            if (EVP_Digest(bytes, size, digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != digest.size())
                return std::nullopt;
            return digest;
        }

        std::optional<Sha256Digest> publicKeyDigest(X509* certificate)
        {
            X509_PUBKEY* const key = X509_get_X509_PUBKEY(certificate);
            const int length = i2d_X509_PUBKEY(key, nullptr);
            if (length <= 0)
                return std::nullopt;
            std::vector<unsigned char> der(static_cast<std::size_t>(length));
            unsigned char* cursor = der.data();
            if (i2d_X509_PUBKEY(key, &cursor) != length)
                return std::nullopt;
            return sha256Of(der.data(), der.size());
        }

        std::optional<Sha256Digest> certificateDigest(X509* certificate)
        {
            Sha256Digest digest{};
            unsigned int length = 0u;
            if (X509_digest(certificate, EVP_sha256(), digest.data(), &length) != 1 || length != digest.size())
                return std::nullopt;
            return digest;
        }

        std::optional<std::string> nameText(X509_NAME const* name)
        {
            const MemoryBio bio{BIO_new(BIO_s_mem())};
            if (!bio || name == nullptr || X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253) < 0)
                return std::nullopt;
            char* data = nullptr;
            const auto size = BIO_get_mem_data(bio.get(), &data);
            if (size < 0)
                return std::nullopt;
            return printable(std::string_view{data, static_cast<std::size_t>(size)});
        }

        std::optional<std::chrono::sys_seconds> timeOf(ASN1_TIME const* time)
        {
            std::tm parts{};
            if (time == nullptr || ASN1_TIME_to_tm(time, &parts) != 1)
                return std::nullopt;
            const std::chrono::year_month_day day{
                std::chrono::year{parts.tm_year + 1900},
                std::chrono::month{static_cast<unsigned>(parts.tm_mon + 1)},
                std::chrono::day{static_cast<unsigned>(parts.tm_mday)},
            };
            if (!day.ok())
                return std::nullopt;
            return std::chrono::sys_days{day} + std::chrono::hours{parts.tm_hour} + std::chrono::minutes{parts.tm_min} +
                std::chrono::seconds{parts.tm_sec};
        }

        std::optional<std::string> addressText(ASN1_OCTET_STRING const* bytes)
        {
            const auto* const data = ASN1_STRING_get0_data(bytes);
            const auto length = ASN1_STRING_length(bytes);
            if (length == 4)
            {
                boost::asio::ip::address_v4::bytes_type address{};
                std::copy_n(data, address.size(), address.begin());
                return boost::asio::ip::address_v4{address}.to_string();
            }
            if (length == 16)
            {
                boost::asio::ip::address_v6::bytes_type address{};
                std::copy_n(data, address.size(), address.begin());
                return boost::asio::ip::address_v6{address}.to_string();
            }
            return std::nullopt;
        }

        std::vector<std::string> alternativeNames(X509* certificate)
        {
            std::vector<std::string> names{};
            const GeneralNames general{
                static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr))
            };
            const int count = general ? sk_GENERAL_NAME_num(general.get()) : 0;
            for (int index = 0; index < count; ++index)
            {
                GENERAL_NAME const* const name = sk_GENERAL_NAME_value(general.get(), index);
                if (name->type == GEN_DNS)
                {
                    const std::string_view text{
                        reinterpret_cast<char const*>(ASN1_STRING_get0_data(name->d.dNSName)),
                        static_cast<std::size_t>(ASN1_STRING_length(name->d.dNSName)),
                    };
                    names.push_back(fmt::format("DNS:{}", printable(text)));
                }
                else if (name->type == GEN_IPADD)
                {
                    if (const auto address = addressText(name->d.iPAddress); address)
                        names.push_back(fmt::format("IP:{}", *address));
                }
            }
            return names;
        }

        /**
         * @brief Never blocks, it runs inside the handshake on an engine thread. A failure is accepted only for a
         *        certificate whose public key matches the pin.
         */
        int verifyPeer(int preverified, X509_STORE_CTX* store)
        {
            auto* const handle =
                static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
            auto* const verification = verificationOf(handle);
            if (verification == nullptr)
                return 0;
            X509* const leaf = X509_STORE_CTX_get0_cert(store);
            if (leaf != nullptr && !verification->leaf && X509_up_ref(leaf) == 1)
                verification->leaf.reset(leaf);
            if (preverified == 1)
                return 1;
            if (verification->firstFailure == X509_V_OK)
                verification->firstFailure = X509_STORE_CTX_get_error(store);
            if (verification->pin && leaf != nullptr && publicKeyDigest(leaf) == verification->pin)
            {
                verification->trustedByPin = true;
                return 1;
            }
            verification->refused = true;
            return 0;
        }

        std::expected<void, RemoteIo::RemoteError> addAuthorities(SSL_CTX* context, std::string const& pem)
        {
            const MemoryBio bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
            if (!bio)
                return std::unexpected(refused("a buffer for an authority"));
            X509_STORE* const store = SSL_CTX_get_cert_store(context);
            std::size_t added = 0u;
            for (
                CertificateHandle certificate{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)}; certificate;
                certificate.reset(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr))
            )
            {
                if (X509_STORE_add_cert(store, certificate.get()) != 1)
                    return std::unexpected(refused("an authority"));
                ++added;
            }
            // The read past the last certificate queued a "no start line".
            ERR_clear_error();
            if (added == 0u)
                return std::unexpected(errorOf(RemoteErrorKind::Failure, "An authority holds no PEM certificate"));
            return {};
        }

        bool isEndOfStream(boost::system::error_code error)
        {
            return error == boost::asio::error::eof || error == boost::asio::ssl::error::stream_truncated ||
                error == boost::asio::error::connection_reset || error == boost::asio::error::connection_aborted;
        }
    }

    std::string formatDigest(Sha256Digest const& digest)
    {
        std::string text{};
        text.reserve(digest.size() * 3u);
        for (std::size_t index = 0u; index < digest.size(); ++index)
        {
            if (index != 0u)
                text.push_back(':');
            text += fmt::format("{:02X}", digest[index]);
        }
        return text;
    }

    std::optional<Sha256Digest> parseDigest(std::string_view text)
    {
        Sha256Digest digest{};
        const bool separated = text.size() == digest.size() * 3u - 1u;
        if (!separated && text.size() != digest.size() * 2u)
            return std::nullopt;
        const std::size_t stride = separated ? 3u : 2u;
        const auto valueOf = [](char character) -> std::optional<std::uint8_t>
        {
            if (character >= '0' && character <= '9')
                return static_cast<std::uint8_t>(character - '0');
            if (character >= 'a' && character <= 'f')
                return static_cast<std::uint8_t>(character - 'a' + 10);
            if (character >= 'A' && character <= 'F')
                return static_cast<std::uint8_t>(character - 'A' + 10);
            return std::nullopt;
        };
        for (std::size_t index = 0u; index < digest.size(); ++index)
        {
            const auto position = index * stride;
            if (separated && index != 0u && text[position - 1u] != ':')
                return std::nullopt;
            const auto high = valueOf(text[position]);
            const auto low = valueOf(text[position + 1u]);
            if (!high || !low)
                return std::nullopt;
            digest[index] = static_cast<std::uint8_t>((*high << 4u) | *low);
        }
        return digest;
    }

    std::optional<CertificateInfo> describeCertificate(X509* certificate)
    {
        if (certificate == nullptr)
            return std::nullopt;
        auto subject = nameText(X509_get_subject_name(certificate));
        auto issuer = nameText(X509_get_issuer_name(certificate));
        const auto notBefore = timeOf(X509_get0_notBefore(certificate));
        const auto notAfter = timeOf(X509_get0_notAfter(certificate));
        const auto certificateSha256 = certificateDigest(certificate);
        const auto publicKeySha256 = publicKeyDigest(certificate);
        if (!subject || !issuer || !notBefore || !notAfter || !certificateSha256 || !publicKeySha256)
            return std::nullopt;
        return CertificateInfo{
            .subject = std::move(*subject),
            .issuer = std::move(*issuer),
            .notBefore = *notBefore,
            .notAfter = *notAfter,
            .subjectAlternativeNames = alternativeNames(certificate),
            .certificateSha256 = *certificateSha256,
            .publicKeySha256 = *publicKeySha256,
        };
    }

    std::expected<std::shared_ptr<TlsContext>, RemoteIo::RemoteError> TlsContext::create(TlsContextOptions options)
    {
        ContextHandle native{SSL_CTX_new(TLS_client_method())};
        if (!native)
            return std::unexpected(refused("a client context"));
        const int minimum = options.minimumVersion == TlsVersion::Tls13 ? TLS1_3_VERSION : TLS1_2_VERSION;
        if (SSL_CTX_set_min_proto_version(native.get(), minimum) != 1)
            return std::unexpected(refused("the minimum TLS version"));
        SSL_CTX_set_session_cache_mode(native.get(), SSL_SESS_CACHE_OFF);
        // Asio's ssl::stream can keep the ClientHello of a renegotiation in its buffer while a write is in flight,
        // both sides then wait. TLS 1.3 has none, TLS 1.2 servers ask only when configured to.
        SSL_CTX_set_options(native.get(), SSL_OP_NO_RENEGOTIATION);
        SSL_CTX_set_verify(native.get(), SSL_VERIFY_PEER, &verifyPeer);
        if (options.systemAuthorities && SSL_CTX_set_default_verify_paths(native.get()) != 1)
            return std::unexpected(refused("the authorities of the system"));
        for (auto const& authority : options.authorities)
        {
            if (const auto added = addAuthorities(native.get(), authority); !added)
                return std::unexpected(added.error());
        }
        // Returns 0 on success, unlike the rest of OpenSSL.
        if (SSL_CTX_set_alpn_protos(native.get(), applicationProtocols.data(), applicationProtocols.size()) != 0)
            return std::unexpected(refused("ALPN"));
        return std::make_shared<TlsContext>(Private{}, native.release());
    }

    TlsContext::TlsContext(Private, SSL_CTX* adopted)
        : context_{adopted}
    {}

    TlsContext::~TlsContext() = default;

    boost::asio::ssl::context& TlsContext::context()
    {
        return context_;
    }

    void SslDeleter::operator()(SSL* handle) const noexcept
    {
        SSL_free(handle);
    }

    void SslSessionDeleter::operator()(SSL_SESSION* session) const noexcept
    {
        SSL_SESSION_free(session);
    }

    std::expected<SslHandle, RemoteIo::RemoteError> makeTlsHandle(TlsConnectionSettings const& settings)
    {
        if (!settings.context)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "TLS was asked for without a TLS context"));
        if (settings.serverName.empty())
            return std::unexpected(
                errorOf(RemoteErrorKind::Failure, "The server has no name to check its certificate against")
            );
        const auto index = verificationIndex();
        if (index < 0)
            return std::unexpected(refused("a place for the certificate check"));
        SslHandle handle{SSL_new(settings.context->context().native_handle())};
        if (!handle)
            return std::unexpected(refused("a connection handle"));
        auto verification = std::make_unique<Verification>();
        verification->pin = settings.pinnedPublicKey;
        if (SSL_set_ex_data(handle.get(), index, verification.get()) != 1)
            return std::unexpected(refused("the certificate check"));
        // Freed with the handle by freeVerification().
        std::ignore = verification.release();

        boost::system::error_code notAnAddress{};
        std::ignore = boost::asio::ip::make_address(settings.serverName, notAnAddress);
        if (!notAnAddress)
        {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(handle.get()), settings.serverName.c_str()) != 1)
                return std::unexpected(refused("the address check"));
            return handle;
        }
        if (SSL_set_tlsext_host_name(handle.get(), settings.serverName.c_str()) != 1)
            return std::unexpected(refused("the server name indication"));
        if (SSL_set1_host(handle.get(), settings.serverName.c_str()) != 1)
            return std::unexpected(refused("the name check"));
        // RFC 6125 6.4.3 lets a client refuse "f*.example.com", browsers do.
        SSL_set_hostflags(handle.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        return handle;
    }

    std::expected<TrustedCertificate, RemoteIo::RemoteError> trustOf(SSL* handle)
    {
        auto const* const verification = verificationOf(handle);
        if (verification == nullptr)
            return std::unexpected(
                errorOf(RemoteErrorKind::NoSecureConnection, "The connection has no certificate check")
            );
        // The verify callback does not run for a resumed session, it would read as trusted by the authorities.
        if (SSL_session_reused(handle) == 1)
            return std::unexpected(errorOf(
                RemoteErrorKind::NoSecureConnection, "The handshake resumed a session, it checked no certificate"
            ));
        const CertificateHandle certificate{SSL_get1_peer_certificate(handle)};
        auto info = describeCertificate(certificate.get());
        if (!info)
            return std::unexpected(
                errorOf(RemoteErrorKind::NoSecureConnection, "The server's certificate cannot be read")
            );
        if (verification->firstFailure == X509_V_OK)
            return TrustedCertificate{.certificate = std::move(*info), .source = TrustSource::Authorities};
        info->verifyFailure = X509_verify_cert_error_string(verification->firstFailure);
        if (!verification->trustedByPin)
            return std::unexpected(
                errorOf(RemoteErrorKind::NoSecureConnection, "The certificate check let a refused one pass")
            );
        return TrustedCertificate{.certificate = std::move(*info), .source = TrustSource::Pin};
    }

    RemoteIo::RemoteError handshakeError(SSL* handle, boost::system::error_code error, bool timedOut)
    {
        if (auto const* const verification = verificationOf(handle); verification != nullptr && verification->refused)
        {
            return errorOf(
                RemoteErrorKind::TrustRequired,
                fmt::format(
                    "The server's certificate is not trusted: {}",
                    X509_verify_cert_error_string(verification->firstFailure)
                )
            );
        }
        if (timedOut)
            return errorOf(RemoteErrorKind::NoSecureConnection, "The TLS handshake did not end in time");
        if (isEndOfStream(error))
            return errorOf(
                RemoteErrorKind::NoSecureConnection, "The server ended the connection during the TLS handshake"
            );
        return errorOf(
            RemoteErrorKind::NoSecureConnection, fmt::format("The TLS handshake failed: {}", error.message())
        );
    }

    RemoteIo::RemoteError asNoSecureConnection(RemoteIo::RemoteError error)
    {
        if (error.kind == RemoteErrorKind::TrustRequired || error.kind == RemoteErrorKind::Cancelled)
            return error;
        error.kind = RemoteErrorKind::NoSecureConnection;
        return error;
    }

    std::optional<CertificateInfo> refusedCertificate(SSL* handle)
    {
        auto const* const verification = verificationOf(handle);
        if (verification == nullptr || !verification->refused || !verification->leaf)
            return std::nullopt;
        auto info = describeCertificate(verification->leaf.get());
        if (info)
            info->verifyFailure = X509_verify_cert_error_string(verification->firstFailure);
        return info;
    }

    std::optional<SslSessionHandle> resumableSession(SSL* handle)
    {
        SSL_SESSION* const current = SSL_get0_session(handle);
        if (current == nullptr || SSL_SESSION_is_resumable(current) != 1)
            return std::nullopt;
        SslSessionHandle copy{SSL_SESSION_dup(current)};
        if (!copy)
            return std::nullopt;
        return copy;
    }
}
