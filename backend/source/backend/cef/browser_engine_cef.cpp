#include <backend/browser_engine.hpp>

#include <include/cef_version_info.h>

#include <fmt/format.h>

std::string browserEngineDescription()
{
    // The entries of the loaded libcef, which is not necessarily the one of the headers (system CEF).
    return fmt::format(
        "Chromium {}.{}.{}.{} (CEF {}.{}.{})",
        cef_version_info(4),
        cef_version_info(5),
        cef_version_info(6),
        cef_version_info(7),
        cef_version_info(0),
        cef_version_info(1),
        cef_version_info(2)
    );
}
