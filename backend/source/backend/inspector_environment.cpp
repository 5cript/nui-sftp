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
#else
    constexpr std::array<char const*, 2> inspectorVariables{
        "WEBKIT_INSPECTOR_SERVER",
        "WEBKIT_INSPECTOR_HTTP_SERVER",
    };
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
}
