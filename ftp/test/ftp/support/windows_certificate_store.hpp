#pragma once

#include <string>

namespace FileTransferProtocol::Test
{
    /**
     * @brief A certificate store of the current user that only the test sees, removed with this. Adding to it asks
     *        nothing, unlike the ROOT store.
     */
    class TemporaryCertificateStore
    {
      public:
        TemporaryCertificateStore();
        ~TemporaryCertificateStore();
        TemporaryCertificateStore(TemporaryCertificateStore const&) = delete;
        TemporaryCertificateStore& operator=(TemporaryCertificateStore const&) = delete;
        TemporaryCertificateStore(TemporaryCertificateStore&&) = delete;
        TemporaryCertificateStore& operator=(TemporaryCertificateStore&&) = delete;

        /**
         * @return Whether @p pem, a certificate in PEM, is in the store now.
         */
        bool add(std::string const& pem);

        wchar_t const* name() const;

      private:
        std::wstring name_;
    };
}
