#include <nui-file-explorer/support/windows_drives.hpp>
#include <paths/native.hpp>
#include <paths/utf8.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace NuiFileExplorer
{
    namespace
    {
        std::string driveDisplayName(std::wstring const& rootPath)
        {
            wchar_t label[MAX_PATH + 1] = {};
            if (GetVolumeInformationW(rootPath.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0))
            {
                std::wstring wide{label};
                if (!wide.empty())
                {
                    // "My Label (C:)"
                    const auto letter = Paths::nativeToUtf8(std::filesystem::path{rootPath}.root_name());
                    return fmt::format("{} ({})", Paths::wideToUtf8(wide), letter);
                }
            }
            // Fallback: just the drive letter
            return Paths::nativeToUtf8(std::filesystem::path{rootPath}.root_name());
        }
    }

    WindowsDrivesProvider::WindowsDrivesProvider(Nui::RpcHub& hub)
        : hub_{&hub}
    {
        registerRpc();
    }
    WindowsDrivesProvider::~WindowsDrivesProvider() = default;

    void WindowsDrivesProvider::registerRpc()
    {
        listDrives_ = std::make_unique<Nui::RpcHub::AutoUnregister>(hub_->autoRegisterFunction(
            "NuiFileExplorer::Drives::list",
            [hub = hub_](std::string responseId)
            {
                nlohmann::json result = nlohmann::json::array();

                const DWORD mask = GetLogicalDrives();
                for (int idx = 0; idx < 26; ++idx)
                {
                    if (!(mask & (1u << idx)))
                        continue;

                    wchar_t rootBuf[4] = {static_cast<wchar_t>('A' + idx), L':', L'\\', L'\0'};
                    std::wstring root{rootBuf};

                    const UINT driveType = GetDriveTypeW(root.c_str());
                    if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN)
                        continue;

                    auto displayName = driveDisplayName(root);
                    auto genericPath = Paths::nativeToUtf8(std::filesystem::path{root});

                    result.push_back({
                        {"name", displayName},
                        {"path", genericPath},
                    });
                }
                hub->callRemote(responseId, {{"success", true}, {"drives", result}});
            }
        ));
    }
}
