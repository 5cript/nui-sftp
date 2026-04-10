#pragma once

#include <nui-file-explorer/item.hpp>
#include <nui-file-explorer/flavor.hpp>
#include <nui-file-explorer/places_provider_interface.hpp>
#include <nui-file-explorer/favorites_provider_interface.hpp>
#include <nui/event_system/observed_value.hpp>
#include <nui-file-explorer/context_menu_item.hpp>

#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace NuiFileExplorer
{
    class ISideModel
    {
      public:
        virtual ~ISideModel() = default;
        ISideModel() = default;
        ISideModel(ISideModel const&) = default;
        ISideModel(ISideModel&&) = default;
        ISideModel& operator=(ISideModel const&) = default;
        ISideModel& operator=(ISideModel&&) = default;

        /**
         * @brief Returns the current path.
         *
         * @return Nui::Observed<std::filesystem::path> const&
         */
        virtual Nui::Observed<std::filesystem::path> const& currentPath() const = 0;

        virtual void setItemUpdateFunction(std::function<void(bool, bool)> doUpdate) = 0;

        /**
         * @brief Returns true if this side model represents the left side.
         *
         * @return bool
         */
        virtual bool isLeft() const = 0;

        /**
         * @brief Navigate to the given (full) path.
         *
         * @param path
         */
        virtual void navigateTo(std::filesystem::path const& path) = 0;

        /**
         * @brief Called when an item is double clicked or ENTER is pressed while an item is selected.
         *
         * @param callback
         */
        virtual void onActivateItem(Item const& item) = 0;

        /**
         * @brief Called when a new item is requested to be created.
         *
         * @param callback
         */
        virtual void onNewItem(Item::Type type) = 0;

        /**
         * @brief Called by side class to get the current items.
         *
         * @param callback
         */
        virtual const std::vector<Item>& items() const = 0;

        /**
         * @brief Set a callback for when the user enters a new path (not triggered by the setPath method).
         *
         * @param callback Called when the user enters a new path (not triggered by the setPath method).
         */
        virtual void onPathChange(std::filesystem::path const& path) = 0;

        /**
         * @brief Set a callback for when the refresh button is clicked.
         *
         * @param callback Called when the refresh button is clicked
         */
        virtual void onRefresh() = 0;

        /**
         * @brief Triggered when items are requested to be deleted.
         *
         * @param callback
         */
        virtual void onDelete(std::vector<Item> const& items) = 0;

        /**
         * @brief Triggered when item is requested to be renamed.
         *
         * @param callback
         */
        virtual void onRename(Item const& item) = 0;

        /**
         * @brief Triggered when item is requested to be queried for attrs.
         *
         * @param callback
         */
        virtual void onProperties(Item const& item) = 0;

        /**
         * @brief Triggered when items are requested to be downloaded / uploaded.
         *
         * @param subDir Optional sub-directory within the current path to transfer into.
         */
        virtual void onTransfer(std::vector<Item> const& items, std::optional<std::string> const& subDir) = 0;

        /**
         * @brief Triggered when items are requested to be downloaded / uploaded via drop.
         *
         * @param subDir Optional sub-directory within the current path to transfer into.
         * @param issueWebkitWarning Whether to show a warning that dropping external items is not fully supported on
         * WebKit-based engines due to technical limitations and bugs.
         */
        virtual void onDropExternal(
            std::vector<Item> const& items,
            std::optional<std::string> const& subDir,
            bool issueWebkitWarning
        ) = 0;

        /**
         * @brief Triggered when an error occurs.
         */
        virtual void onError(std::string const& error) = 0;

        /**
         * @brief Create a list of suggestions for the path box based on the given path.
         * This should be fast and expected to be called on every key press. The output should be limited to the given
         * number of entries only.
         *
         * @param path The path to generate suggestions for
         * @param maxSuggestions The maximum number of suggestions to return (0 = no limit)
         * @param onResultsAvailable Callback to call when results are available
         */
        virtual void generatePathBoxSuggestions(
            std::filesystem::path const& path,
            int maxSuggestions,
            std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
        ) = 0;

        /**
         * @brief Store some metadata that is to be attached to file drops from the desktop for
         * postMessageWithAdditionalObjects in webview2.
         *
         * @param data
         */
        virtual void dropMetadata(std::string const& data) = 0;

        /**
         * @brief Retrieve the metadata that is to be attached to file drops from the desktop for
         * postMessageWithAdditionalObjects in webview2.
         *
         * @return std::string
         */
        virtual std::string dropMetadata() const = 0;

        /**
         * @brief Goes back to the previous path, if any.
         */
        virtual void goBack() = 0;

        /**
         * @brief Returns the context menu items for the given selection.
         * Called just before the context menu opens; the returned entries
         * are used directly as PopupMenu items, allowing each side model to
         * expose exactly the actions that make sense for its context.
         *
         * @param selectedItems The items currently selected (may be empty).
         * @return std::vector<ContextMenuItem>
         */
        virtual std::vector<ContextMenuItem> contextMenuItems(std::vector<Item> const& selectedItems) = 0;

        /**
         * @brief Optional interface: provides default system places (XDG / Windows known folders).
         *
         * @return Pointer to the provider or nullptr if not supported.
         */
        virtual IPlacesProvider* placesProvider()
        {
            return nullptr;
        }

        /**
         * @brief Optional interface: provides drive/volume entries (Windows-specific).
         *
         * @return Pointer to the provider or nullptr if not supported.
         */
        virtual IDrivesProvider* drivesProvider()
        {
            return nullptr;
        }

        /**
         * @brief Optional interface: manages user-defined favorite paths.
         *
         * @return Pointer to the provider or nullptr if not supported.
         */
        virtual IFavoritesProvider* favoritesProvider()
        {
            return nullptr;
        }
    };
}