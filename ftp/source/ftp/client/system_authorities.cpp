#include <ftp/client/system_authorities.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include "openssl_error.hpp"

#include <fmt/format.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <string>
#include <system_error>

namespace FileTransferProtocol
{
#if !defined(_WIN32) && !defined(__APPLE__)
    namespace
    {
        using Detail::refusedByOpenSsl;

        bool environmentChooses()
        {
            return std::getenv(X509_get_default_cert_file_env()) != nullptr ||
                std::getenv(X509_get_default_cert_dir_env()) != nullptr;
        }

        std::expected<void, RemoteIo::RemoteError> addReplacementBundle(SSL_CTX* context)
        {
            if (environmentChooses())
                return {};
            const auto bundle = replacementBundleOf(authorityBundlesOfThisSystem());
            if (!bundle)
                return {};
            const auto path = Paths::nativeToUtf8(*bundle);
            Log::info("FTP: OpenSSL's authorities are missing, trusting the ones in {}.", path);
            if (SSL_CTX_load_verify_locations(context, path.c_str(), nullptr) != 1)
                return std::unexpected(refusedByOpenSsl(fmt::format("the authorities in {}", path)));
            return {};
        }
    }
#endif

    AuthorityBundles authorityBundlesOfThisSystem()
    {
        return AuthorityBundles{
            .defaultFile = Paths::nativeFromUtf8(X509_get_default_cert_file()),
            .known = {
                "/etc/ssl/certs/ca-certificates.crt",
                "/etc/pki/tls/certs/ca-bundle.crt",
                "/etc/ssl/ca-bundle.pem",
                "/etc/pki/tls/cacert.pem",
                "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                "/etc/ssl/cert.pem",
            },
        };
    }

    std::optional<std::filesystem::path> replacementBundleOf(AuthorityBundles const& bundles)
    {
        std::error_code error{};
        if (std::filesystem::is_regular_file(bundles.defaultFile, error))
            return std::nullopt;
        for (auto const& bundle : bundles.known)
        {
            if (std::filesystem::is_regular_file(bundle, error))
                return bundle;
        }
        return std::nullopt;
    }

#if defined(_WIN32) || defined(__APPLE__)
    std::expected<void, RemoteIo::RemoteError> addSystemAuthorities(SSL_CTX*)
    {
        return {};
    }
#else
    std::expected<void, RemoteIo::RemoteError> addSystemAuthorities(SSL_CTX* context)
    {
        if (SSL_CTX_set_default_verify_paths(context) != 1)
            return std::unexpected(refusedByOpenSsl("the authorities of the system"));
        return addReplacementBundle(context);
    }
#endif
}
