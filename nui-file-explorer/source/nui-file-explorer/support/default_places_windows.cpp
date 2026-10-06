#include <nui-file-explorer/support/default_places.hpp>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

namespace NuiFileExplorer
{
    namespace
    {
        std::string resolveKnownFolder(KNOWNFOLDERID const& folderId)
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &path)))
            {
                std::wstring wide{path};
                CoTaskMemFree(path);
                return std::filesystem::path{wide}.generic_string();
            }
            return {};
        }

        struct FolderDef
        {
            char const* kind;
            char const* name;
            KNOWNFOLDERID const* folderId;
        };

        FolderDef const windowsFolders[] = {
            {"home", "Home", &FOLDERID_Profile},
            {"desktop", "Desktop", &FOLDERID_Desktop},
            {"downloads", "Downloads", &FOLDERID_Downloads},
            {"documents", "Documents", &FOLDERID_Documents},
            {"music", "Music", &FOLDERID_Music},
            {"pictures", "Pictures", &FOLDERID_Pictures},
            {"videos", "Videos", &FOLDERID_Videos},
        };
    }

    DefaultPlacesProvider::DefaultPlacesProvider(Nui::RpcHub& hub)
        : hub_{&hub}
    {
        registerRpc();
    }
    DefaultPlacesProvider::~DefaultPlacesProvider() = default;

    void DefaultPlacesProvider::registerRpc()
    {
        listPlaces_ = std::make_unique<Nui::RpcHub::AutoUnregister>(hub_->autoRegisterFunction(
            "NuiFileExplorer::DefaultPlaces::list",
            [hub = hub_](std::string responseId)
            {
                nlohmann::json result = nlohmann::json::array();

                for (auto const& def : windowsFolders)
                {
                    auto resolved = resolveKnownFolder(*def.folderId);
                    if (resolved.empty())
                        continue;
                    result.push_back({
                        {"kind", def.kind},
                        {"name", def.name},
                        {"path", resolved},
                    });
                }
                hub->callRemote(responseId, {{"success", true}, {"places", result}});
            }
        ));
    }
}
