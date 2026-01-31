#include <backend/windows/main_windows.hpp>

Main::PlatformSpecifics::PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub)
    : enableDragDrop{window, hub}
{}