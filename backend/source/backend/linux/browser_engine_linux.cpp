#include <backend/browser_engine.hpp>

#if __has_include(<webkit/webkit.h>)
#    include <webkit/webkit.h>
#else
#    include <webkit2/webkit2.h>
#endif

#include <fmt/format.h>

std::string browserEngineDescription()
{
    return fmt::format(
        "WebKitGTK {}.{}.{}", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version()
    );
}
