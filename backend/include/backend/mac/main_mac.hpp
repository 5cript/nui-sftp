#pragma once

#include <backend/mac/web_view_hooks.hpp>
#include <backend/main.hpp>

struct Main::PlatformSpecifics
{
    PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub);

    MacWebViewHooks webViewHooks;
};
