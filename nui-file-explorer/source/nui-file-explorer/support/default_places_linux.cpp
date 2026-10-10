#include <nui-file-explorer/support/default_places.hpp>
#include <utility/user_directories.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace NuiFileExplorer
{
    namespace
    {
        struct PlaceDefinition
        {
            char const* kind;
            char const* name;
            /**
             * @brief The name in user-dirs.dirs, such as DESKTOP for XDG_DESKTOP_DIR. nullptr when XDG has none.
             */
            char const* userDirectoryName;
            /**
             * @brief Relative to home, used when neither the environment nor user-dirs.dirs name the directory.
             */
            char const* fallback;
        };

        constexpr PlaceDefinition placeDefinitions[] = {
            {"desktop", "Desktop", "DESKTOP", "Desktop"},
            {"downloads", "Downloads", "DOWNLOAD", "Downloads"},
            {"documents", "Documents", "DOCUMENTS", "Documents"},
            {"pictures", "Pictures", "PICTURES", "Pictures"},
            {"videos", "Videos", "VIDEOS", "Videos"},
            {"movies", "Movies", nullptr, "Movies"},
            {"music", "Music", "MUSIC", "Music"},
        };

        std::filesystem::path environmentPath(char const* name)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0')
                return {};
            return std::filesystem::path{value};
        }

        std::vector<Utility::UserDirectory> readUserDirectories(std::filesystem::path const& home)
        {
            auto configHome = environmentPath("XDG_CONFIG_HOME");
            if (configHome.empty())
                configHome = home / ".config";

            std::ifstream file{configHome / "user-dirs.dirs", std::ios::binary};
            if (!file)
                return {};
            const std::string content{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            return Utility::parseUserDirectories(content, home);
        }

        /**
         * @brief Where a place is, or an empty path when it is disabled. A user-dirs.dirs file with entries is
         *        authoritative: places it does not name are left out, like the remote side does.
         */
        std::filesystem::path resolvePlace(
            PlaceDefinition const& definition,
            std::filesystem::path const& home,
            std::vector<Utility::UserDirectory> const& userDirectories
        )
        {
            if (definition.userDirectoryName != nullptr)
            {
                auto fromEnvironment = environmentPath(fmt::format("XDG_{}_DIR", definition.userDirectoryName).c_str());
                if (!fromEnvironment.empty())
                    return fromEnvironment;

                const auto userDirectory =
                    std::ranges::find(userDirectories, definition.userDirectoryName, &Utility::UserDirectory::name);
                if (userDirectory != userDirectories.end())
                    return userDirectory->path;
            }
            if (!userDirectories.empty())
                return {};
            return home / definition.fallback;
        }

        nlohmann::json listPlaces()
        {
            nlohmann::json result = nlohmann::json::array();
            const auto home = environmentPath("HOME");
            if (home.empty())
                return result;

            result.push_back({{"kind", "home"}, {"name", "Home"}, {"path", home.generic_string()}});

            const auto userDirectories = readUserDirectories(home);
            for (auto const& definition : placeDefinitions)
            {
                const auto path = resolvePlace(definition, home, userDirectories);
                std::error_code error;
                if (path.empty() || !std::filesystem::is_directory(path, error))
                    continue;
                result.push_back(
                    {{"kind", definition.kind}, {"name", definition.name}, {"path", path.generic_string()}}
                );
            }
            return result;
        }
    }

    DefaultPlacesProvider::DefaultPlacesProvider(Nui::RpcHub& hub)
        : hub_{&hub}
        , listPlaces_{std::make_unique<Nui::RpcHub::AutoUnregister>(hub.autoRegisterFunction(
              "NuiFileExplorer::DefaultPlaces::list",
              [hubPtr = &hub](std::string responseId)
              {
                  hubPtr->callRemote(responseId, {{"success", true}, {"places", listPlaces()}});
              }
          ))}
    {}
    DefaultPlacesProvider::~DefaultPlacesProvider() = default;

    void DefaultPlacesProvider::registerRpc()
    {}
}
