#pragma once

#include <expected>
#include <functional>
#include <string>

namespace FileTransferProtocol::Test
{
    /**
     * @brief A certificate and the key it certifies, both in PEM.
     */
    struct CertifiedKey
    {
        std::string certificate{};
        std::string privateKey{};
    };

    /**
     * @brief Certificates for the tests, made at run time so no checked-in key expires. The server certificates name
     *        127.0.0.1, ::1 and localhost unless they say otherwise, and meet Apple's rules for TLS servers: server
     *        authentication as extended key usage, valid for a year.
     */
    struct TestPki
    {
        /** @brief The certificate of the test authority, the one a client trusts. */
        std::string authority{};
        /** @brief Signed by the test authority. */
        CertifiedKey server{};
        /** @brief Signed by the test authority with a key of its own, a data connection that differs from control. */
        CertifiedKey otherServer{};
        /** @brief Signed by itself. */
        CertifiedKey selfSigned{};
        /** @brief Signed by the test authority, valid until yesterday. */
        CertifiedKey expired{};
        /** @brief Signed by the test authority for another name. */
        CertifiedKey wrongName{};
        /** @brief Signed by the test authority for f*.nui-sftp.test, a partial wildcard. */
        CertifiedKey partialWildcard{};
        /** @brief Signed by the test authority for *.nui-sftp.test. */
        CertifiedKey wildcard{};
        /** @brief Signed by an authority no client trusts. */
        CertifiedKey otherAuthority{};
    };

    /**
     * @brief The PKI of this process, made on first use.
     *
     * @return The PKI, or why OpenSSL could not make it.
     */
    std::expected<std::reference_wrapper<TestPki const>, std::string> testPki();
}
