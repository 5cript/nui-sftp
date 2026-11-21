#include "windows_certificate_store.hpp"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        std::vector<unsigned char> derOf(std::string const& pem)
        {
            const std::unique_ptr<BIO, decltype(&BIO_free)> bio{
                BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free
            };
            const std::unique_ptr<X509, decltype(&X509_free)> certificate{
                PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free
            };
            if (!certificate)
                return {};
            const int length = i2d_X509(certificate.get(), nullptr);
            if (length <= 0)
                return {};
            std::vector<unsigned char> der(static_cast<std::size_t>(length));
            unsigned char* output = der.data();
            if (i2d_X509(certificate.get(), &output) != length)
                return {};
            return der;
        }

        std::wstring uniqueName()
        {
            static std::atomic<unsigned> counter{0u};
            return L"NuiSftpTestAuthorities-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter.fetch_add(1u));
        }

        struct CloseStore
        {
            void operator()(void* store) const noexcept
            {
                CertCloseStore(static_cast<HCERTSTORE>(store), 0);
            }
        };
    }

    TemporaryCertificateStore::TemporaryCertificateStore()
        : name_{uniqueName()}
    {}

    TemporaryCertificateStore::~TemporaryCertificateStore()
    {
        CertUnregisterSystemStore(name_.c_str(), CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_DELETE_FLAG);
    }

    bool TemporaryCertificateStore::add(std::string const& pem)
    {
        const auto der = derOf(pem);
        if (der.empty())
            return false;
        const std::unique_ptr<void, CloseStore> store{
            CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, name_.c_str())
        };
        if (!store)
            return false;
        return CertAddEncodedCertificateToStore(
                   static_cast<HCERTSTORE>(store.get()),
                   X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                   der.data(),
                   static_cast<DWORD>(der.size()),
                   CERT_STORE_ADD_REPLACE_EXISTING,
                   nullptr
               ) == TRUE;
    }

    wchar_t const* TemporaryCertificateStore::name() const
    {
        return name_.c_str();
    }
}
