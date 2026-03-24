#pragma once

#include <string_view>

namespace Constants
{
#ifdef NDEBUG
    constexpr static std::string_view persistencePath = "%config_home2%/nui-sftp/persistence.json";
#else
    constexpr static std::string_view persistencePath = "%config_home2%/nui-sftp/persistence.devel.json";
#endif
    constexpr static std::string_view appName = "nui-sftp";
    constexpr static std::string_view defaultThemeName = "Base Green";
}