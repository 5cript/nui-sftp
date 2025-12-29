#pragma once

#include <nui-file-explorer/item.hpp>
#include <nui-file-explorer/flavor.hpp>
#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/side/side_settings.hpp>
#include <nui-file-explorer/side/side_implementation.hpp>
#include <nui-file-explorer/side/icon_flavor.hpp>
#include <nui-file-explorer/side/table_flavor.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/api/mouse_event.hpp>

#include <memory>

namespace NuiFileExplorer
{
    class Side
    {
      public:
        friend class FlavorImplementation;
        friend class IconFlavor;
        friend class TableFlavor;

        /**
         * @brief Construct a new side given settings and model.
         *
         * @param settings
         * @param model
         */
        Side(SideSettings settings, std::unique_ptr<ISideModel> model);
        ~Side();
        Side(const Side&) = delete;
        Side& operator=(const Side&) = delete;
        Side(Side&&);
        Side& operator=(Side&&);

        ISideModel& model();

        /**
         * @brief Pulls items from model and displays them.
         */
        void updateItems(bool sorted);

        /**
         * @brief Determines how the grid should be displayed.
         */
        void flavor(Flavor value);

        /**
         * @brief Returns the current grid flavor.
         */
        Flavor flavor() const;

        /**
         * @brief Sets the size of the icons in the grid.
         */
        void iconSize(unsigned int value);

        /**
         * @brief Sets the spacing between icons in the grid.
         */
        void iconSpacing(unsigned int value);

        /**
         * @brief Returns the size of the icons in the grid.
         */
        unsigned int iconSize() const;

        /**
         * @brief Returns the spacing between icons in the grid.
         */
        unsigned int iconSpacing() const;

        /**
         * @brief Returns all selected items.
         */
        std::vector<Item> selectedItems() const;

        /**
         * @brief Returns the paths of all selected items.
         */
        std::vector<std::filesystem::path> selectedPaths() const;

        /**
         * @brief Use this to close all menus and deselect all items.
         */
        void onUneventfulClick();

        /**
         * @brief Set the Path in an input box.
         */
        void path(std::filesystem::path const& path);

        /**
         * @brief Returns the current path.
         *
         * @return std::filesystem::path
         */
        std::filesystem::path path();

        /**
         * @brief Closes all menus without deselecting items.
         */
        void closeMenus();

        Nui::ElementRenderer operator()();

      private:
        Nui::ElementRenderer headMenu();
        Nui::ElementRenderer pathBar();
        Nui::ElementRenderer filter();
        Nui::ElementRenderer contextMenu();
        void onContextMenu(ItemWithInternals* item, Nui::val event);
        void onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent event);
        void processKeyboardEvent(Nui::WebApi::KeyboardEvent event);
        void search(std::string query);

      private:
        std::unique_ptr<SideImplementation> impl_;
        IconFlavor iconFlavor_;
        TableFlavor tableFlavor_;
    };
}