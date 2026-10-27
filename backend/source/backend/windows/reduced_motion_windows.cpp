#include <backend/reduced_motion.hpp>
#include <log/log.hpp>
#include <paths/utf8.hpp>

#include <WebView2.h>
#include <wrl/event.h>

#include <string>

template <>
_GUID const& __mingw_uuidof<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>()
{
    return IID_ICoreWebView2CallDevToolsProtocolMethodCompletedHandler;
}

void applyReducedMotion(Nui::Window& window, Persistence::ReducedMotion preference)
{
    window.dispatch(
        [&window, preference]()
        {
            auto* webView = static_cast<ICoreWebView2*>(window.getNativeWebView());
            if (webView == nullptr)
            {
                Log::warn("Cannot apply the reduced motion setting, the window has no web view.");
                return;
            }

            const auto method = Paths::utf8ToWide(reducedMotionEmulationMethod);
            const auto parameters = Paths::utf8ToWide(reducedMotionEmulationParameters(preference).dump());
            const auto result = webView->CallDevToolsProtocolMethod(
                method.c_str(),
                parameters.c_str(),
                Microsoft::WRL::Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                    [](HRESULT errorCode, LPCWSTR returnObjectAsJson) -> HRESULT
                    {
                        if (FAILED(errorCode))
                        {
                            Log::error(
                                "Failed to apply the reduced motion setting over the DevTools protocol: {:#x} {}",
                                static_cast<unsigned long>(errorCode),
                                returnObjectAsJson == nullptr ? std::string{} : Paths::wideToUtf8(returnObjectAsJson)
                            );
                        }
                        return S_OK;
                    }
                ).Get()
            );
            if (FAILED(result))
                Log::error(
                    "Failed to apply the reduced motion setting over the DevTools protocol: {:#x}",
                    static_cast<unsigned long>(result)
                );
        }
    );
}
