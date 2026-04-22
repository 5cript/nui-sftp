#pragma once

#include <shared_data/shared_data.hpp>

#include <string>

namespace SharedData
{
    /**
     *  @brief Result of a runtime probe of the platform's "open file externally" facilities.
     *  @details On Linux this reflects whether xdg-desktop-portal's OpenURI interface (and,
     *           under flatpak, the Documents portal it depends on) are reachable. On Windows
     *           these are always @c true -- ShellExecuteW has no discoverable failure mode
     *           ahead of the call.
     */
    struct OpenerCapabilities
    {
        bool canOpenFile{true};
        bool canOpenInFileManager{true};
        /// Empty on success; a short human-readable diagnostic otherwise (suitable for UI display).
        std::string reason{};
    };
    BOOST_DESCRIBE_STRUCT(OpenerCapabilities, (), (canOpenFile, canOpenInFileManager, reason))
}
