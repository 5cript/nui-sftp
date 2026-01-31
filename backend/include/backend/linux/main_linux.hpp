#pragma once

#include <backend/main.hpp>

struct Main::PlatformSpecifics
{
    PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub);
};