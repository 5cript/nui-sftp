#include <ftp/client/system_authorities.hpp>
#include <log/log.hpp>

#include <openssl/err.h>
#include <openssl/x509.h>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace FileTransferProtocol
{
    namespace
    {
        struct CloseStore
        {
            void operator()(void* store) const noexcept
            {
                CertCloseStore(static_cast<HCERTSTORE>(store), 0);
            }
        };
        using StoreHandle = std::unique_ptr<void, CloseStore>;

        struct FreeCertificate
        {
            void operator()(X509* certificate) const noexcept
            {
                X509_free(certificate);
            }
        };
        using CertificateHandle = std::unique_ptr<X509, FreeCertificate>;

        /**
         * @brief Whether @p certificate has no extended key usage, which allows every usage, or one that names server
         *        authentication.
         */
        bool allowsServerAuthentication(PCCERT_CONTEXT certificate)
        {
            DWORD size = 0;
            if (CertGetEnhancedKeyUsage(certificate, 0, nullptr, &size) == FALSE)
                return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
            std::vector<BYTE> buffer(size);
            auto* const usage = reinterpret_cast<CERT_ENHKEY_USAGE*>(buffer.data());
            if (CertGetEnhancedKeyUsage(certificate, 0, usage, &size) == FALSE)
                return false;
            // No identifiers: valid for all usages when no property was found, otherwise for none.
            if (usage->cUsageIdentifier == 0u)
                return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
            const std::span identifiers{usage->rgpszUsageIdentifier, usage->cUsageIdentifier};
            for (char const* identifier : identifiers)
            {
                if (identifier != nullptr && std::strcmp(identifier, szOID_PKIX_KP_SERVER_AUTH) == 0)
                    return true;
            }
            return false;
        }

        bool usableForServers(PCCERT_CONTEXT certificate)
        {
            return CertVerifyTimeValidity(nullptr, certificate->pCertInfo) == 0 &&
                allowsServerAuthentication(certificate);
        }

        std::size_t addCertificatesOf(HCERTSTORE system, X509_STORE* store)
        {
            std::size_t added = 0u;
            PCCERT_CONTEXT certificate = CertEnumCertificatesInStore(system, nullptr);
            while (certificate != nullptr)
            {
                if (usableForServers(certificate))
                {
                    unsigned char const* encoded = certificate->pbCertEncoded;
                    const CertificateHandle decoded{
                        d2i_X509(nullptr, &encoded, static_cast<long>(certificate->cbCertEncoded))
                    };
                    if (decoded && X509_STORE_add_cert(store, decoded.get()) == 1)
                        ++added;
                }
                // Frees the context it is given.
                certificate = CertEnumCertificatesInStore(system, certificate);
            }
            // Certificates OpenSSL could not read or had already leave errors that are no failure.
            ERR_clear_error();
            return added;
        }
    }

    std::size_t addStoreAuthorities(X509_STORE* store, std::span<wchar_t const* const> storeNames)
    {
        std::size_t added = 0u;
        for (wchar_t const* name : storeNames)
        {
            const StoreHandle system{CertOpenStore(
                CERT_STORE_PROV_SYSTEM_W,
                0,
                0,
                CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
                name
            )};
            if (!system)
            {
                Log::warn("FTP: The Windows certificate store could not be opened: error {}.", GetLastError());
                continue;
            }
            added += addCertificatesOf(static_cast<HCERTSTORE>(system.get()), store);
        }
        return added;
    }
}
