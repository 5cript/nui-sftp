#pragma once

#include <backend/main.hpp>
#include <backend/windows/windows_drag_drop.hpp>

struct Main::PlatformSpecifics
{
    PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub);

    EnableWindowsDragDrop enableDragDrop;
};