#include <ftp/client/platform_trust.hpp>
#include <log/log.hpp>
#include "platform_answer.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>

#include <array>
#include <memory>
#include <optional>
#include <type_traits>

namespace FileTransferProtocol
{
    namespace
    {
        struct ReleaseReference
        {
            void operator()(void const* reference) const noexcept
            {
                CFRelease(reference);
            }
        };
        template <typename ReferenceT>
        using Reference = std::unique_ptr<std::remove_pointer_t<ReferenceT>, ReleaseReference>;

        /**
         * @return Nothing when a certificate cannot be read.
         */
        std::optional<Reference<CFMutableArrayRef>> certificatesOf(std::vector<DerCertificate> const& certificates)
        {
            Reference<CFMutableArrayRef> array{CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks)};
            if (!array)
                return std::nullopt;
            for (auto const& der : certificates)
            {
                const Reference<CFDataRef> data{CFDataCreate(nullptr, der.data(), static_cast<CFIndex>(der.size()))};
                if (!data)
                    return std::nullopt;
                const Reference<SecCertificateRef> certificate{SecCertificateCreateWithData(nullptr, data.get())};
                if (!certificate)
                    return std::nullopt;
                CFArrayAppendValue(array.get(), certificate.get());
            }
            return array;
        }

        /**
         * @brief Evaluates @p trust on a queue of its own and hands the result to @p answer. Takes the trust.
         */
        void evaluate(Reference<SecTrustRef> trust, std::shared_ptr<Detail::PlatformAnswer> answer)
        {
            // Released once the evaluation answered.
            dispatch_queue_t queue = dispatch_queue_create("nui-sftp.platform-trust", DISPATCH_QUEUE_SERIAL);
            SecTrustRef const evaluated = trust.release();
            // SecTrustEvaluateAsyncWithError has to be called on the queue it answers on.
            dispatch_async(queue, ^{
              const auto started =
                  SecTrustEvaluateAsyncWithError(evaluated, queue, ^(SecTrustRef, bool trusted, CFErrorRef error) {
                    if (!trusted && error != nullptr)
                    {
                        const Reference<CFStringRef> description{CFErrorCopyDescription(error)};
                        std::array<char, 512> text{};
                        if (description &&
                            CFStringGetCString(description.get(), text.data(), text.size(), kCFStringEncodingUTF8))
                            Log::info("FTP: The system does not trust the certificate: {}", text.data());
                    }
                    answer->give(trusted);
                    CFRelease(evaluated);
                    dispatch_release(queue);
                  });
              if (started != errSecSuccess)
              {
                  Log::error("FTP: The system could not evaluate the certificate: OSStatus {}.", started);
                  answer->give(false);
                  CFRelease(evaluated);
                  dispatch_release(queue);
              }
            });
        }
    }

    PlatformVerdict platformVerdictOf(
        std::vector<DerCertificate> const& chain,
        std::string const& serverName,
        std::stop_token const& stop,
        std::optional<std::vector<DerCertificate>> const& exclusiveAnchors
    )
    {
        if (chain.empty())
            return PlatformVerdict::Refused;
        const auto certificates = certificatesOf(chain);
        if (!certificates)
            return PlatformVerdict::Refused;
        const Reference<CFStringRef> name{
            CFStringCreateWithCString(nullptr, serverName.c_str(), kCFStringEncodingUTF8)
        };
        const Reference<SecPolicyRef> policy{SecPolicyCreateSSL(true, name.get())};
        SecTrustRef created = nullptr;
        if (!name || !policy ||
            SecTrustCreateWithCertificates(certificates->get(), policy.get(), &created) != errSecSuccess)
            return PlatformVerdict::Refused;
        Reference<SecTrustRef> trust{created};
        if (exclusiveAnchors)
        {
            const auto anchorCertificates = certificatesOf(*exclusiveAnchors);
            if (!anchorCertificates ||
                SecTrustSetAnchorCertificates(trust.get(), anchorCertificates->get()) != errSecSuccess ||
                SecTrustSetAnchorCertificatesOnly(trust.get(), true) != errSecSuccess)
                return PlatformVerdict::Refused;
        }
        SecTrustSetNetworkFetchAllowed(trust.get(), true);

        const auto answer = std::make_shared<Detail::PlatformAnswer>();
        evaluate(std::move(trust), answer);
        const auto trusted = answer->waitUntilGiven(stop);
        return trusted && *trusted ? PlatformVerdict::Trusted : PlatformVerdict::Refused;
    }
}
