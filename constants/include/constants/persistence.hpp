#pragma once

#include <string_view>

namespace Constants
{
#ifdef NDEBUG
    constexpr static std::string_view persistencePath = "%config_home2%/nui-sftp/persistence.json";
    constexpr static std::string_view commandStorePath = "%data_home2%/nui-sftp/command_store.db";
    constexpr static std::string_view trustedKeysPath = "%config_home2%/nui-sftp/ftp_trusted_keys.json";
#else
    constexpr static std::string_view persistencePath = "%config_home2%/nui-sftp/persistence.devel.json";
    constexpr static std::string_view commandStorePath = "%data_home2%/nui-sftp/command_store.devel.db";
    constexpr static std::string_view trustedKeysPath = "%config_home2%/nui-sftp/ftp_trusted_keys.devel.json";
#endif
    constexpr static std::string_view appName = "nui-sftp";
    constexpr static std::string_view defaultThemeName = "Base Green";
}