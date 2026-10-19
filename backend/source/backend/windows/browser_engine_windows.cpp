#include <backend/browser_engine.hpp>

#include <paths/utf8.hpp>

#include <windows.h>

#include <array>
#include <string>

namespace
{
    // The WebView2 runtime in EdgeUpdate, as in Microsoft's "Detect if a WebView2 Runtime is already installed".
    constexpr wchar_t const* machineRuntimeKey =
        L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    constexpr wchar_t const* userRuntimeKey =
        L"Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

    std::wstring runtimeVersion(HKEY root, wchar_t const* key)
    {
        std::array<wchar_t, 64> buffer{};
        auto size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        if (RegGetValueW(root, key, L"pv", RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
            return {};
        const std::wstring version{buffer.data()};
        return version == L"0.0.0.0" ? std::wstring{} : version;
    }
}

std::string browserEngineDescription()
{
    auto version = runtimeVersion(HKEY_LOCAL_MACHINE, machineRuntimeKey);
    if (version.empty())
        version = runtimeVersion(HKEY_CURRENT_USER, userRuntimeKey);
    if (version.empty())
        return "WebView2";
    return "WebView2 " + Paths::wideToUtf8(version);
}
