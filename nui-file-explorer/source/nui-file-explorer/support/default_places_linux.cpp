#include <nui-file-explorer/support/default_places.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace NuiFileExplorer
{
    namespace
    {
        std::filesystem::path xdgDir(char const* envVar, char const* fallbackSuffix)
        {
            const char* val = std::getenv(envVar);
            if (val && val[0] != '\0')
                return std::filesystem::path{val};
            const char* home = std::getenv("HOME");
            if (home && home[0] != '\0')
                return std::filesystem::path{home} / fallbackSuffix;
            return {};
        }

        struct PlaceDef
        {
            char const* name;
            char const* envVar;   // nullptr for Home (uses HOME directly)
            char const* fallback; // relative to HOME
        };

        constexpr PlaceDef xdgPlaces[] = {
            {"Home",      nullptr,            ""},
            {"Desktop",   "XDG_DESKTOP_DIR",  "Desktop"},
            {"Downloads", "XDG_DOWNLOAD_DIR",  "Downloads"},
            {"Documents", "XDG_DOCUMENTS_DIR", "Documents"},
            {"Music",     "XDG_MUSIC_DIR",     "Music"},
            {"Pictures",  "XDG_PICTURES_DIR",  "Pictures"},
            {"Videos",    "XDG_VIDEOS_DIR",    "Videos"},
        };
    }

    DefaultPlacesProvider::DefaultPlacesProvider(Nui::RpcHub& hub)
        : hub_{&hub}
        , listPlaces_{hub.autoRegisterFunction(
              "NuiFileExplorer::DefaultPlaces::list",
              [hubPtr = &hub](std::string responseId)
              {
                  nlohmann::json result = nlohmann::json::array();

                  for (auto const& def : xdgPlaces)
                  {
                      std::filesystem::path resolved;
                      if (def.envVar == nullptr)
                      {
                          const char* home = std::getenv("HOME");
                          if (home && home[0] != '\0')
                              resolved = std::filesystem::path{home};
                      }
                      else
                      {
                          resolved = xdgDir(def.envVar, def.fallback);
                      }

                      if (resolved.empty())
                          continue;

                      result.push_back({
                          {"name", def.name},
                          {"path", resolved.generic_string()},
                      });
                  }
                  hubPtr->callRemote(responseId, {{"success", true}, {"places", result}});
              }
          )}
    {}
    DefaultPlacesProvider::~DefaultPlacesProvider() = default;

    void DefaultPlacesProvider::registerRpc()
    {}
}
