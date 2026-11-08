#pragma once

#include <ftp/client/tls.hpp>
#include <ftp/support/test_pki.hpp>

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        struct CertificateFree
        {
            void operator()(X509* certificate) const noexcept
            {
                X509_free(certificate);
            }
        };
        using CertificateHandle = std::unique_ptr<X509, CertificateFree>;

        CertificateHandle readCertificate(std::string const& pem)
        {
            std::unique_ptr<BIO, decltype(&BIO_free)> bio{
                BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free
            };
            return CertificateHandle{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
        }

        /**
         * @brief The SHA-256 of the DER public key the way i2d_PUBKEY writes it from the key, not from the certificate.
         */
        std::optional<Sha256Digest> publicKeyDigestFromKey(X509* certificate)
        {
            EVP_PKEY* const key = X509_get0_pubkey(certificate);
            unsigned char* der = nullptr;
            const int length = i2d_PUBKEY(key, &der);
            if (length <= 0)
                return std::nullopt;
            Sha256Digest digest{};
            unsigned int size = 0u;
            const bool hashed =
                EVP_Digest(der, static_cast<std::size_t>(length), digest.data(), &size, EVP_sha256(), nullptr) == 1;
            OPENSSL_free(der);
            if (!hashed)
                return std::nullopt;
            return digest;
        }

        TestPki const& pki()
        {
            const auto made = testPki();
            if (!made)
                throw std::runtime_error{made.error()};
            return made->get();
        }
    }

    TEST(TlsDigestTests, FormatsAsColonSeparatedUpperCaseHex)
    {
        Sha256Digest digest{};
        for (std::size_t index = 0u; index < digest.size(); ++index)
            digest[index] = static_cast<std::uint8_t>(index * 9u);
        const auto text = formatDigest(digest);
        EXPECT_EQ(text.substr(0u, 12u), "00:09:12:1B:");
        EXPECT_EQ(text.size(), 32u * 3u - 1u);
    }

    TEST(TlsDigestTests, ParsesWhatItFormatsAndPlainHexInAnyCase)
    {
        Sha256Digest digest{};
        for (std::size_t index = 0u; index < digest.size(); ++index)
            digest[index] = static_cast<std::uint8_t>(255u - index);
        const auto formatted = formatDigest(digest);
        EXPECT_EQ(parseDigest(formatted), digest);

        std::string plain{};
        std::ranges::copy_if(
            formatted,
            std::back_inserter(plain),
            [](char character)
            {
                return character != ':';
            }
        );
        std::ranges::transform(
            plain,
            plain.begin(),
            [](char character)
            {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
        );
        EXPECT_EQ(parseDigest(plain), digest);
    }

    TEST(TlsDigestTests, RefusesWrongLengthsAndOtherCharacters)
    {
        EXPECT_FALSE(parseDigest(""));
        EXPECT_FALSE(parseDigest(std::string(62u, 'a')));
        EXPECT_FALSE(parseDigest(std::string(66u, 'a')));
        EXPECT_FALSE(parseDigest(std::string(63u, 'a') + "g"));
        EXPECT_FALSE(parseDigest(std::string(63u, 'a') + " "));
        EXPECT_TRUE(parseDigest(std::string(64u, 'a')));
    }

    TEST(TlsCertificateTests, DescribesTheServerCertificateOfTheTestAuthority)
    {
        const auto certificate = readCertificate(pki().server.certificate);
        ASSERT_TRUE(certificate);
        const auto info = describeCertificate(certificate.get());
        ASSERT_TRUE(info);

        EXPECT_NE(info->subject.find("CN="), std::string::npos) << info->subject;
        EXPECT_NE(info->issuer, info->subject);
        const auto now = std::chrono::system_clock::now();
        EXPECT_LT(info->notBefore, now);
        EXPECT_GT(info->notAfter, now);
        EXPECT_NE(
            std::ranges::find(info->subjectAlternativeNames, "IP:127.0.0.1"), info->subjectAlternativeNames.end()
        );
        EXPECT_NE(std::ranges::find(info->subjectAlternativeNames, "IP:::1"), info->subjectAlternativeNames.end());
        EXPECT_NE(
            std::ranges::find(info->subjectAlternativeNames, "DNS:localhost"), info->subjectAlternativeNames.end()
        );
        EXPECT_TRUE(info->verifyFailure.empty());
    }

    TEST(TlsCertificateTests, PublicKeyDigestCoversTheWholeSubjectPublicKeyInfo)
    {
        const auto certificate = readCertificate(pki().server.certificate);
        ASSERT_TRUE(certificate);
        const auto info = describeCertificate(certificate.get());
        ASSERT_TRUE(info);
        EXPECT_EQ(info->publicKeySha256, publicKeyDigestFromKey(certificate.get()));
        EXPECT_NE(info->publicKeySha256, info->certificateSha256);
    }

    TEST(TlsCertificateTests, CertificatesWithOneKeyShareThePublicKeyDigestOnly)
    {
        const auto server = readCertificate(pki().server.certificate);
        const auto other = readCertificate(pki().otherServer.certificate);
        ASSERT_TRUE(server);
        ASSERT_TRUE(other);
        const auto serverInfo = describeCertificate(server.get());
        const auto otherInfo = describeCertificate(other.get());
        ASSERT_TRUE(serverInfo);
        ASSERT_TRUE(otherInfo);
        EXPECT_NE(serverInfo->publicKeySha256, otherInfo->publicKeySha256);
        EXPECT_NE(serverInfo->certificateSha256, otherInfo->certificateSha256);
    }

    TEST(TlsCertificateTests, NothingToDescribeWithoutACertificate)
    {
        EXPECT_FALSE(describeCertificate(nullptr));
    }

    TEST(TlsContextTests, TakesAuthoritiesInPem)
    {
        const auto context = TlsContext::create({.systemAuthorities = false, .authorities = {pki().authority}});
        ASSERT_TRUE(context) << context.error().toString();
        EXPECT_NE((*context)->context().native_handle(), nullptr);
    }

    TEST(TlsContextTests, RefusesAnAuthorityWithoutACertificate)
    {
        const auto context = TlsContext::create({.systemAuthorities = false, .authorities = {"not a certificate"}});
        ASSERT_FALSE(context);
        EXPECT_EQ(context.error().kind, RemoteIo::RemoteErrorKind::Failure);
    }

    TEST(TlsContextTests, HandleNeedsAServerName)
    {
        const auto context = TlsContext::create({.systemAuthorities = false});
        ASSERT_TRUE(context) << context.error().toString();
        const auto handle = makeTlsHandle({.context = *context, .serverName = ""});
        ASSERT_FALSE(handle);
        EXPECT_EQ(handle.error().kind, RemoteIo::RemoteErrorKind::Failure);
        EXPECT_TRUE(makeTlsHandle({.context = *context, .serverName = "127.0.0.1"}));
        EXPECT_TRUE(makeTlsHandle({.context = *context, .serverName = "ftp.example.org"}));
    }
}
