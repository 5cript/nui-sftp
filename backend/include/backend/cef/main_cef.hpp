#pragma once

#include <backend/cef/cef_hooks.hpp>
#include <backend/main.hpp>

struct Main::PlatformSpecifics
{
    PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub);

    CefHooks cefHooks;
};
