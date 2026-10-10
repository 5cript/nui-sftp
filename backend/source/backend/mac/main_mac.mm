#include <backend/mac/main_mac.hpp>

Main::PlatformSpecifics::PlatformSpecifics(Nui::Window& window, Nui::RpcHub& hub)
    : webViewHooks{window, hub}
{}
