#include <backend/inspector_environment.hpp>

#include <log/log.hpp>

#ifdef _WIN32
#    include <windows.h>
#endif

#include <array>
#include <cstdlib>
#include <string_view>

namespace
{
#ifdef _WIN32
    constexpr std::array<char const*, 2> inspectorVariables{
        "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        "WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER",
    };
#elif defined(NUI_ENGINE_CEF)
    // Nui reads the port only for windows with debug enabled, the variable is removed all the same.
    constexpr std::array<char const*, 1> inspectorVariables{
        "NUI_CEF_REMOTE_DEBUGGING_PORT",
    };
#else
    constexpr std::array<char const*, 2> inspectorVariables{
        "WEBKIT_INSPECTOR_SERVER",
        "WEBKIT_INSPECTOR_HTTP_SERVER",
    };
#endif

#ifdef NUI_ENGINE_CEF
    /// Chromium switches for every Nui window, a remote debugging switch among them serves DevTools as well.
    constexpr char const* chromiumArgumentsVariable = "NUI_CEF_ARGUMENTS";
#endif

    void removeVariable(char const* name)
    {
#ifdef _WIN32
        _putenv_s(name, "");
        SetEnvironmentVariableA(name, nullptr);
#else
        unsetenv(name);
#endif
    }
}

void removeRemoteInspectorEnvironment()
{
    for (auto const* name : inspectorVariables)
    {
        char const* value = std::getenv(name);
        if (value == nullptr)
            continue;

        Log::warn("Removed {}='{}' from the environment, pass --enable-dev-tools to keep it.", name, std::string_view{value});
        removeVariable(name);
    }

#ifdef NUI_ENGINE_CEF
    char const* const arguments = std::getenv(chromiumArgumentsVariable);
    if (arguments != nullptr && std::string_view{arguments}.find("--remote-debugging") != std::string_view::npos)
    {
        Log::warn(
            "Removed {}='{}' from the environment, pass --enable-dev-tools to keep it.",
            chromiumArgumentsVariable,
            std::string_view{arguments}
        );
        removeVariable(chromiumArgumentsVariable);
    }
#endif
}
