#pragma once

#include <backend/main.hpp>
#include <backend/windows/windows_drag_drop.hpp>

struct Main::PlatformSpecifics
{
    PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub);

    EnableWindowsDragDrop enableDragDrop;
};

/**
 * @brief Attaches to the console of the process that started this one, if there is one, and points the standard
 *        streams at it. The program is a GUI program, without this its output goes nowhere.
 */
void attachParentConsole();