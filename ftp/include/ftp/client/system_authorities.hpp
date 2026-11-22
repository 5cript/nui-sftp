#pragma once

#include <remote_io/remote_error.hpp>

#include <openssl/ssl.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <vector>

namespace FileTransferProtocol
{
    /**
     * @brief The file OpenSSL loads the authorities of the system from, and the bundles Linux distributions keep them
     *        in.
     */
    struct AuthorityBundles
    {
        /** @brief OpenSSL's default file, which an OpenSSL of another distribution may not find. */
        std::filesystem::path defaultFile{};
        /** @brief The bundles of the known distributions, in the order they are tried. */
        std::vector<std::filesystem::path> known{};
    };

    /**
     * @brief OpenSSL's default file and the bundles of Debian, Fedora, RHEL, openSUSE, OpenELEC and Alpine.
     */
    AuthorityBundles authorityBundlesOfThisSystem();

    /**
     * @brief The bundle to load in place of a missing default file, as for an AppImage that brings the OpenSSL of
     *        another distribution: the first known one that exists.
     *
     * @return Nothing when the default file exists or no known bundle does.
     */
    std::optional<std::filesystem::path> replacementBundleOf(AuthorityBundles const& bundles);

    /**
     * @brief Trusts the authorities of the system in @p context, on Linux: OpenSSL's default places, and a known bundle
     *        in place of a missing default file unless SSL_CERT_FILE or SSL_CERT_DIR choose. On macOS and Windows it
     *        adds none: the system itself decides there, with what it distrusts, about every chain OpenSSL refused
     *        for it, see platformVerdictOf().
     *
     * @return A Failure naming what OpenSSL refused.
     */
    std::expected<void, RemoteIo::RemoteError> addSystemAuthorities(SSL_CTX* context);
}
