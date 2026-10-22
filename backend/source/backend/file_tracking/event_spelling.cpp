#include <backend/file_tracking/event_spelling.hpp>

#ifdef __APPLE__
#    include <CoreFoundation/CoreFoundation.h>

#    include <cstddef>
#    include <memory>
#    include <type_traits>
#endif

namespace FileTracking
{
#ifdef __APPLE__
    namespace
    {
        struct CoreFoundationRelease
        {
            void operator()(void const* object) const
            {
                CFRelease(object);
            }
        };

        template <typename Reference>
        using CoreFoundationHandle = std::unique_ptr<std::remove_pointer_t<Reference>, CoreFoundationRelease>;

        /**
         * @brief @p text in NFC the way efsw precomposes it (CFStringNormalize), unchanged when it is not UTF-8.
         */
        std::string precomposed(std::string const& text)
        {
            const CoreFoundationHandle<CFStringRef> source{CFStringCreateWithBytes(
                kCFAllocatorDefault,
                reinterpret_cast<UInt8 const*>(text.data()),
                static_cast<CFIndex>(text.size()),
                kCFStringEncodingUTF8,
                false
            )};
            if (!source)
                return text;
            const CoreFoundationHandle<CFMutableStringRef> normalized{
                CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source.get())
            };
            if (!normalized)
                return text;
            CFStringNormalize(normalized.get(), kCFStringNormalizationFormC);
            const auto range = CFRangeMake(0, CFStringGetLength(normalized.get()));
            CFIndex byteCount{0};
            CFStringGetBytes(normalized.get(), range, kCFStringEncodingUTF8, 0, false, nullptr, 0, &byteCount);
            std::string result(static_cast<std::size_t>(byteCount), '\0');
            CFStringGetBytes(
                normalized.get(),
                range,
                kCFStringEncodingUTF8,
                0,
                false,
                reinterpret_cast<UInt8*>(result.data()),
                byteCount,
                nullptr
            );
            return result;
        }
    }
#endif

    std::string eventSpelling(std::string const& name)
    {
#ifdef __APPLE__
        return precomposed(name);
#else
        return name;
#endif
    }
}
