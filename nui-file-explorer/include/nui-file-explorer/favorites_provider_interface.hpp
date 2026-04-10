#pragma once

#include <nui/event_system/observed_value.hpp>

#include <filesystem>
#include <memory>
#include <vector>

namespace NuiFileExplorer
{
    /**
     * @brief Optional interface: manages user-defined favorite paths. Side models that want to
     * expose a Favorites section should return @c this from @c ISideModel::favoritesProvider().
     *
     * The concrete model is responsible for persisting the list across sessions.
     */
    class IFavoritesProvider
    {
      public:
        virtual ~IFavoritesProvider() = default;

        /**
         * @brief Returns a shared pointer to the observable favorites list for reactive UI binding.
         *
         * @return Shared ownership so the UI component can safely hold the observed value
         *         independently of the model's lifetime.
         */
        virtual std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites() const = 0;

        /**
         * @brief Adds a path to the favorites list and persists the change.
         *
         * @param path The path to add.
         */
        virtual void addFavorite(std::filesystem::path const& path) = 0;

        /**
         * @brief Removes a path from the favorites list and persists the change.
         *
         * @param path The path to remove.
         */
        virtual void removeFavorite(std::filesystem::path const& path) = 0;
    };
}
