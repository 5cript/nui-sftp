#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace NuiFileExplorer
{
    /**
     * @brief Optional interface: provides the "Places" section entries (system bookmarks such as
     * XDG directories on Linux or Windows known folders on Windows). Side models that want to
     * expose a Places section should return @c this from @c ISideModel::placesProvider().
     */
    class IPlacesProvider
    {
      public:
        struct PlaceEntry
        {
            Nui::ElementRenderer icon;
            std::string name;
            std::filesystem::path path;
        };

        virtual ~IPlacesProvider() = default;

        /**
         * @brief Request the list of default/system places asynchronously.
         *
         * @param callback Called with the resolved list. May be called immediately (synchronously)
         *        if the data is already available.
         */
        virtual void requestDefaultPlaces(std::function<void(std::vector<PlaceEntry>)> callback) = 0;
    };

    /**
     * @brief Optional interface: provides drive/volume entries (Windows-specific). Side models
     * that want to expose a Drives section should return @c this from
     * @c ISideModel::drivesProvider().
     */
    class IDrivesProvider
    {
      public:
        using PlaceEntry = IPlacesProvider::PlaceEntry;

        virtual ~IDrivesProvider() = default;

        /**
         * @brief Request the list of available drives/volumes asynchronously.
         *
         * @param callback Called with the resolved list.
         */
        virtual void requestDrives(std::function<void(std::vector<PlaceEntry>)> callback) = 0;
    };
}
