#include <ftp/support/test_pki.hpp>

#include <fmt/format.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        template <auto release>
        struct Release
        {
            template <typename Resource>
            void operator()(Resource* resource) const noexcept
            {
                release(resource);
            }
        };
        using Key = std::unique_ptr<EVP_PKEY, Release<EVP_PKEY_free>>;
        using Certificate = std::unique_ptr<X509, Release<X509_free>>;
        using Name = std::unique_ptr<X509_NAME, Release<X509_NAME_free>>;
        using Extension = std::unique_ptr<X509_EXTENSION, Release<X509_EXTENSION_free>>;
        using MemoryBio = std::unique_ptr<BIO, Release<BIO_free_all>>;

        constexpr std::string_view allServerNames = "IP:127.0.0.1,IP:::1,DNS:localhost";
        constexpr std::chrono::hours oneDay{24};
        /** @brief Apple refuses a TLS server certificate valid for more than 825 days. */
        constexpr std::chrono::hours oneYear{24 * 365};
        constexpr std::chrono::hours tenYears{24 * 3650};

        std::unexpected<std::string> openSslFailure(std::string_view what)
        {
            std::array<char, 256> reason{};
            ERR_error_string_n(ERR_get_error(), reason.data(), reason.size());
            return std::unexpected(fmt::format("{}: {}", what, reason.data()));
        }

        struct Authority
        {
            Certificate certificate{};
            Key key{};
        };

        struct Request
        {
            std::string_view commonName{};
            /** @brief The subject alternative names of a server, empty for an authority. */
            std::string_view names{};
            std::chrono::hours validFrom{-oneDay};
            std::chrono::hours validUntil{oneYear};
        };

        std::expected<Key, std::string> makeKey()
        {
            Key key{EVP_EC_gen("P-256")};
            if (!key)
                return openSslFailure("Generating a key");
            return key;
        }

        std::expected<void, std::string>
        addExtension(X509V3_CTX& context, X509* certificate, int kind, char const* value)
        {
            const Extension extension{X509V3_EXT_conf_nid(nullptr, &context, kind, value)};
            if (!extension || X509_add_ext(certificate, extension.get(), -1) != 1)
                return openSslFailure(fmt::format("Adding the extension '{}'", value));
            return {};
        }

        /**
         * @brief Makes a certificate of @p key, signed by @p issuer or by itself without one.
         */
        std::expected<Certificate, std::string>
        makeCertificate(Request const& request, EVP_PKEY* key, Authority const* issuer)
        {
            static long nextSerial = 1;

            Certificate certificate{X509_new()};
            if (!certificate)
                return openSslFailure("Creating a certificate");

            // The certificate copies the name, the one it holds is not to be changed.
            const Name subject{X509_NAME_new()};
            if (!subject)
                return openSslFailure("Creating the name of a certificate");
            const std::string commonName{request.commonName};
            const bool valid = X509_set_version(certificate.get(), X509_VERSION_3) == 1 &&
                ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), nextSerial++) == 1 &&
                X509_gmtime_adj(
                    X509_getm_notBefore(certificate.get()),
                    std::chrono::duration_cast<std::chrono::seconds>(request.validFrom).count()
                ) != nullptr &&
                X509_gmtime_adj(
                    X509_getm_notAfter(certificate.get()),
                    std::chrono::duration_cast<std::chrono::seconds>(request.validUntil).count()
                ) != nullptr &&
                X509_set_pubkey(certificate.get(), key) == 1 &&
                X509_NAME_add_entry_by_txt(
                    subject.get(),
                    "CN",
                    MBSTRING_ASC,
                    reinterpret_cast<unsigned char const*>(commonName.c_str()),
                    -1,
                    -1,
                    0
                ) == 1 &&
                X509_set_subject_name(certificate.get(), subject.get()) == 1 &&
                X509_set_issuer_name(
                    certificate.get(), issuer ? X509_get_subject_name(issuer->certificate.get()) : subject.get()
                ) == 1;
            if (!valid)
                return openSslFailure("Filling a certificate");

            X509V3_CTX context{};
            X509V3_set_ctx(
                &context, issuer ? issuer->certificate.get() : certificate.get(), certificate.get(), nullptr, nullptr, 0
            );
            const auto extended = request.names.empty()
                ? addExtension(context, certificate.get(), NID_basic_constraints, "critical,CA:TRUE")
                      .and_then(
                          [&]()
                          {
                              return addExtension(
                                  context, certificate.get(), NID_key_usage, "critical,keyCertSign,cRLSign"
                              );
                          }
                      )
                : addExtension(context, certificate.get(), NID_basic_constraints, "CA:FALSE")
                      .and_then(
                          [&]()
                          {
                              const std::string names{request.names};
                              return addExtension(context, certificate.get(), NID_subject_alt_name, names.c_str());
                          }
                      )
                      .and_then(
                          [&]()
                          {
                              // Apple trusts a TLS server certificate only with this usage.
                              return addExtension(context, certificate.get(), NID_ext_key_usage, "serverAuth");
                          }
                      );
            if (!extended)
                return std::unexpected(extended.error());

            if (X509_sign(certificate.get(), issuer ? issuer->key.get() : key, EVP_sha256()) == 0)
                return openSslFailure("Signing a certificate");
            return certificate;
        }

        std::expected<Authority, std::string> makeAuthority(std::string_view commonName)
        {
            return makeKey().and_then(
                [&](Key key) -> std::expected<Authority, std::string>
                {
                    auto certificate =
                        makeCertificate(Request{.commonName = commonName, .validUntil = tenYears}, key.get(), nullptr);
                    if (!certificate)
                        return std::unexpected(std::move(certificate).error());
                    return Authority{.certificate = std::move(certificate).value(), .key = std::move(key)};
                }
            );
        }

        template <typename Write>
        std::expected<std::string, std::string> toPem(Write write)
        {
            const MemoryBio memory{BIO_new(BIO_s_mem())};
            if (!memory || write(memory.get()) != 1)
                return openSslFailure("Writing PEM");
            char* data = nullptr;
            const auto size = BIO_get_mem_data(memory.get(), &data);
            return std::string(data, static_cast<std::size_t>(size));
        }

        std::expected<std::string, std::string> certificatePem(X509* certificate)
        {
            return toPem(
                [certificate](BIO* memory)
                {
                    return PEM_write_bio_X509(memory, certificate);
                }
            );
        }

        std::expected<CertifiedKey, std::string> makeServer(Request const& request, Authority const* issuer)
        {
            return makeKey().and_then(
                [&](Key key) -> std::expected<CertifiedKey, std::string>
                {
                    const auto certificate = makeCertificate(request, key.get(), issuer);
                    if (!certificate)
                        return std::unexpected(certificate.error());
                    const auto certificateText = certificatePem(certificate->get());
                    const auto keyText = toPem(
                        [&key](BIO* memory)
                        {
                            return PEM_write_bio_PrivateKey(memory, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
                        }
                    );
                    if (!certificateText || !keyText)
                        return std::unexpected(!certificateText ? certificateText.error() : keyText.error());
                    return CertifiedKey{.certificate = *certificateText, .privateKey = *keyText};
                }
            );
        }

        std::expected<TestPki, std::string> makePki()
        {
            const auto authority = makeAuthority("nui-sftp test authority");
            const auto foreign = makeAuthority("nui-sftp foreign authority");
            if (!authority || !foreign)
                return std::unexpected(!authority ? authority.error() : foreign.error());
            const auto authorityText = certificatePem(authority->certificate.get());
            if (!authorityText)
                return std::unexpected(authorityText.error());

            const Request server{.commonName = "nui-sftp test server", .names = allServerNames};
            const Request expired{
                .commonName = "nui-sftp expired server",
                .names = allServerNames,
                .validFrom = -2 * oneDay,
                .validUntil = -oneDay,
            };
            const Request wrongName{.commonName = "nui-sftp other host", .names = "DNS:other.invalid"};
            const Request partialWildcard{.commonName = "nui-sftp partial wildcard", .names = "DNS:f*.nui-sftp.test"};
            const Request wildcard{.commonName = "nui-sftp wildcard", .names = "DNS:*.nui-sftp.test"};

            TestPki pki{.authority = *authorityText};
            const std::array<std::pair<CertifiedKey*, std::expected<CertifiedKey, std::string>>, 8> made{{
                {&pki.server, makeServer(server, &*authority)},
                {&pki.otherServer, makeServer(server, &*authority)},
                {&pki.selfSigned, makeServer(server, nullptr)},
                {&pki.expired, makeServer(expired, &*authority)},
                {&pki.wrongName, makeServer(wrongName, &*authority)},
                {&pki.partialWildcard, makeServer(partialWildcard, &*authority)},
                {&pki.wildcard, makeServer(wildcard, &*authority)},
                {&pki.otherAuthority, makeServer(server, &*foreign)},
            }};
            for (auto const& [target, result] : made)
            {
                if (!result)
                    return std::unexpected(result.error());
                *target = *result;
            }
            return pki;
        }
    }

    std::expected<std::reference_wrapper<TestPki const>, std::string> testPki()
    {
        static const std::expected<TestPki, std::string> pki = makePki();
        if (!pki)
            return std::unexpected(pki.error());
        return std::cref(*pki);
    }

    std::vector<std::uint8_t> derOfPem(std::string const& pem)
    {
        const MemoryBio memory{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
        const Certificate certificate{PEM_read_bio_X509(memory.get(), nullptr, nullptr, nullptr)};
        if (!certificate)
            throw std::runtime_error{"The test certificate cannot be read"};
        const int length = i2d_X509(certificate.get(), nullptr);
        if (length <= 0)
            throw std::runtime_error{"The test certificate cannot be written as DER"};
        std::vector<std::uint8_t> der(static_cast<std::size_t>(length));
        unsigned char* output = der.data();
        if (i2d_X509(certificate.get(), &output) != length)
            throw std::runtime_error{"The test certificate cannot be written as DER"};
        return der;
    }
}
