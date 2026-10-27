#include <backend/reduced_motion.hpp>
#include <log/log.hpp>

#include <nui/cef/client_hooks.hpp>

#include <include/cef_parser.h>

#include <string>

void applyReducedMotion(Nui::Window& window, Persistence::ReducedMotion preference)
{
    window.dispatch(
        [&window, preference]()
        {
            const auto browser = Nui::Cef::browser(window);
            if (!browser)
            {
                Log::warn("Cannot apply the reduced motion setting, the window has no browser.");
                return;
            }

            const auto parameters = CefParseJSON(reducedMotionEmulationParameters(preference).dump(), JSON_PARSER_RFC);
            if (!parameters ||
                browser->GetHost()->ExecuteDevToolsMethod(
                    0, std::string{reducedMotionEmulationMethod}, parameters->GetDictionary()
                ) == 0)
                Log::error("Failed to apply the reduced motion setting over the DevTools protocol.");
        }
    );
}
