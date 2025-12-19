#pragma once

#include <nui-file-explorer/item.hpp>
#include <nui-file-explorer/flavor.hpp>
#include <nui/event_system/observed_value.hpp>

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

        virtual void setItemUpdateFunction(std::function<void(bool)> doUpdate) = 0;

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
         * @brief Triggered when items are requested to be downloaded.
         */
        virtual void onTransfer(std::vector<Item> const& items) = 0;

        /**
         * @brief Triggered when an error occurs.
         */
        virtual void onError(std::string const& error) = 0;
    };
}