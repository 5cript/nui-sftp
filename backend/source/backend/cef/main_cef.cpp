#include <backend/cef/main_cef.hpp>

Main::PlatformSpecifics::PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub)
    : cefHooks{window, hub}
{}
