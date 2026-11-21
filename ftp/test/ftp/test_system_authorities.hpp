#pragma once

#include <ftp/client/platform_trust.hpp>
#include <ftp/client/system_authorities.hpp>
#include <ftp/client/tls.hpp>

#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

extern std::filesystem::path temporaryRoot;

namespace FileTransferProtocol::Test
{
    class AuthorityBundleTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            directory_ =
                temporaryRoot / "authority-bundles" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
            std::filesystem::create_directories(directory_);
        }

        std::filesystem::path existing(std::string const& name) const
        {
            const auto path = directory_ / name;
            std::ofstream writer{path, std::ios_base::binary};
            writer << "-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n";
            return path;
        }

        std::filesystem::path missing(std::string const& name) const
        {
            return directory_ / name;
        }

        std::filesystem::path directory_{};
    };

    TEST_F(AuthorityBundleTests, AnExistingDefaultFileNeedsNoReplacement)
    {
        const AuthorityBundles bundles{.defaultFile = existing("cert.pem"), .known = {existing("known.pem")}};
        EXPECT_FALSE(replacementBundleOf(bundles));
    }

    TEST_F(AuthorityBundleTests, TheFirstKnownBundleThatExistsReplacesAMissingDefaultFile)
    {
        const AuthorityBundles bundles{
            .defaultFile = missing("cert.pem"),
            .known = {missing("debian.crt"), existing("fedora.crt"), existing("alpine.pem")},
        };
        EXPECT_EQ(replacementBundleOf(bundles), bundles.known[1]);
    }

    TEST_F(AuthorityBundleTests, ADirectoryIsNoBundle)
    {
        std::filesystem::create_directories(directory_ / "certs");
        const AuthorityBundles bundles{
            .defaultFile = missing("cert.pem"),
            .known = {directory_ / "certs", existing("fedora.crt")},
        };
        EXPECT_EQ(replacementBundleOf(bundles), bundles.known[1]);
    }

    TEST_F(AuthorityBundleTests, WithoutAKnownBundleNothingReplacesAMissingDefaultFile)
    {
        const AuthorityBundles bundles{.defaultFile = missing("cert.pem"), .known = {missing("debian.crt")}};
        EXPECT_FALSE(replacementBundleOf(bundles));
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    TEST(SystemAuthorityTests, ThisSystemHasAuthoritiesWhereOpenSslLooksOrInAKnownBundle)
    {
        const auto bundles = authorityBundlesOfThisSystem();
        std::error_code error{};
        EXPECT_TRUE(std::filesystem::is_regular_file(bundles.defaultFile, error) || replacementBundleOf(bundles))
            << bundles.defaultFile;
    }
#endif

#if defined(__APPLE__) || defined(_WIN32)
    TEST(SystemAuthorityTests, OpenSslTrustsNoAuthorityWhereTheSystemDecides)
    {
        const auto context = TlsContext::create({});
        ASSERT_TRUE(context) << context.error().toString();
        X509_STORE* const store = SSL_CTX_get_cert_store((*context)->context().native_handle());
        const auto free = [](STACK_OF(X509)* certificates)
        {
            sk_X509_pop_free(certificates, X509_free);
        };
        const std::unique_ptr<STACK_OF(X509), decltype(free)> trusted{X509_STORE_get1_all_certs(store), free};
        ASSERT_TRUE(trusted);
        EXPECT_EQ(sk_X509_num(trusted.get()), 0);
    }
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
    TEST(PlatformTrustTests, OtherPlatformsHaveNoCheckOfTheirOwn)
    {
        EXPECT_EQ(platformVerdictOf({DerCertificate{0x30u, 0x00u}}, "127.0.0.1", {}), PlatformVerdict::Unavailable);
    }
#endif
}
