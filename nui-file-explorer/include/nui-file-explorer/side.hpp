#pragma once

#include <nui-file-explorer/item.hpp>
#include <nui-file-explorer/flavor.hpp>
#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/side/side_settings.hpp>
#include <nui-file-explorer/side/side_implementation.hpp>
#include <nui-file-explorer/side/icon_flavor.hpp>
#include <nui-file-explorer/side/places.hpp>
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

        /**
         * @brief Initialize passing a pointer to the other side, so that they can interact with each other if needed.
         *
         * @param otherSide Other side of the file grid. Its optionally can be nullptr if the file grid only has one
         * side.
         */
        void initialize(Side* otherSide);

        ISideModel& model();

        /**
         * @brief Pulls items from model and displays them.
         */
        void updateItems(bool sorted, bool reapplySelection);

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
         * @brief Whether hidden files are currently shown on this side.
         */
        bool showHiddenFiles() const;

        /**
         * @brief Apply the hidden-file visibility setting.  Invokes the
         *        SideSettings::onShowHiddenFilesChanged callback and re-sorts
         *        visible items — same semantic as toggling via the head-menu
         *        button, minus the user-gesture source.
         */
        void showHiddenFiles(bool value);

        /**
         * @brief Current sort criterion and ascending flag (criterion, ascending).
         */
        std::pair<SortCriterion, bool> sort() const;

        /**
         * @brief Apply a sort criterion + direction and re-sort in place.
         */
        void sort(std::pair<SortCriterion, bool> value);

        /**
         * @brief Places panel open/closed state.  std::nullopt means "follow
         *        the responsive width default", which the ResizeObserver flips
         *        between wide (inline) and narrow (popup) modes.
         */
        std::optional<bool> placesOpen() const;

        /**
         * @brief Force the places panel into the given open state, or clear
         *        the override to fall back to responsive width default.
         */
        void placesOpen(std::optional<bool> value);

        /**
         * @brief Whether the hosting element is currently wide enough for the
         *        inline (always-open) places-panel layout.  Derived from
         *        ResizeObserver; not writable by callers.
         */
        bool placesWide() const;

        /**
         * @brief Returns all selected items.
         */
        std::vector<Item> selectedItems() const;

        /**
         * @brief Replaces the texts the side renders, for example after a language change. Syncing the change is
         * left to the caller.
         */
        void texts(Texts value);

        /**
         * @brief The texts the side currently renders.
         */
        Texts const& texts() const;

        /**
         * @brief Sets the user and host the origin card names. Syncing the change is left to the caller.
         */
        void originHost(std::string value);

        /**
         * @brief Sets a callback invoked when both sides have exactly one directory-like item
         *        selected and the user triggers the "Synchronize..." context menu entry.
         *        The callback receives (localPath, remotePath) regardless of which side's
         *        context menu was opened.
         *
         * @param callback Called with (localPath, remotePath).
         */
        void setOnSynchronize(
            std::function<void(std::filesystem::path localPath, std::filesystem::path remotePath)>
                callback
        );

        /**
         * @brief Returns the paths of all selected items.
         */
        std::vector<std::filesystem::path> selectedPaths() const;

        /**
         * @brief Overlays the shown items at @p fullPaths with a trash icon until the next listing replaces them.
         *        Syncing the change is left to the caller.
         *
         * @param fullPaths The full paths of the items whose delete is queued.
         */
        void markPendingDelete(std::vector<std::filesystem::path> const& fullPaths);

        /**
         * @brief Closes the path suggestions.
         */
        void closePathSuggestions(bool otherSideToo = false) const;

        /**
         * @brief Use this to close all menus and deselect all items.
         */
        void onUneventfulClick();

        /**
         * @brief Set the Path in an input box.
         */
        void path(std::filesystem::path const& path);

        /**
         * @brief Navigates to @p directory and selects the item @p name there once the listing has loaded, as a
         *        single click on it does: the selection is replaced, the keyboard continues from the item, the item
         *        is scrolled into view and the side takes the focus. Navigating elsewhere before the listing arrives
         *        cancels the selection, a listing without the item selects nothing.
         *
         * @param directory The directory to show.
         * @param name The file name of the item in @p directory.
         */
        void navigateAndSelect(std::filesystem::path const& directory, std::filesystem::path const& name);

        /**
         * @brief Returns the current path.
         *
         * @return std::filesystem::path
         */
        std::filesystem::path path();

        /**
         * @brief Closes all menus without deselecting items.
         *
         * @return Returns whether or not the context menu was closed.
         */
        bool closeMenus();

        /**
         * @brief Access the places panel, might return nullptr
         *
         * @return Places*
         */
        Places* places();

        Nui::ElementRenderer operator()();

      private:
        Nui::ElementRenderer originIconAndKind();
        std::string originClass(std::string const& baseClass) const;
        Nui::ElementRenderer originChip();
        Nui::ElementRenderer originCard();
        Nui::ElementRenderer headMenu();
        Nui::ElementRenderer pathBar();
        Nui::ElementRenderer filter();
        Nui::ElementRenderer pathBarSuggestions();
        void onContextMenu(ItemWithInternals* item, Nui::val event);
        void onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent event);
        void processKeyboardEvent(Nui::WebApi::KeyboardEvent event);

        /**
         * @brief Puts @p items on this side's clipboard, @p cut moves them when they are pasted on this side. Only a
         *        side that accepts drops on itself can cut.
         */
        void copyToClipboard(std::vector<Item> items, bool cut);

        /**
         * @brief The clipboard a paste on this side takes: the one of either side copied or cut to last, nullptr when
         *        there is nothing this side can paste.
         */
        SideImplementation::Clipboard* pasteableClipboard();

        /**
         * @brief Pastes what was copied or cut last on either side: items of the other side are transferred, items of
         *        this side are copied or moved within it.
         *
         * @param into The directory that takes the items, the directory this side shows when unset.
         */
        void pasteClipboard(std::optional<Item> const& into);
        void search(std::string query);
        void onPathBoxSuggestionHit(std::filesystem::path const& path);

        /**
         *  @brief Mark the side as currently loading a directory listing. Schedules a debounced
         *         visibility hint so fast navigations don't flash a placeholder.
         */
        void beginNavigationLoading();

        /**
         *  @brief Cancel any pending loading-hint timer and clear the loading state.
         */
        void clearNavigationLoading();

        /**
         *  @brief Selects the item requested by @ref navigateAndSelect when the listing just shown is of its
         *         directory, and ends the request.
         */
        void selectPendingItem();

        /**
         *  @brief Commit the search box value as a filter that hides non-matching items.
         *         Used only in paginated mode; for small directories the live highlight path
         *         in @ref search is used instead.
         */
        void applyFilter(std::string query);

        /**
         *  @brief Append @p key to the type-ahead buffer, (re)start the idle timer, and
         *         jump to the first item whose filename starts with the accumulated buffer
         *         (case-insensitive). The match is highlighted using the existing search
         *         highlight so it looks the same as a search box hit.
         */
        void handleTypeAheadKey(std::string const& key);

        /**
         *  @brief Clear the type-ahead buffer and any type-ahead highlight. Leaves the
         *         current selection in place.
         */
        void clearTypeAhead();

      private:
        Nui::ElementRenderer placesPanel();

      private:
        std::unique_ptr<SideImplementation> impl_;
        std::unique_ptr<IconFlavor> iconFlavor_;
        std::unique_ptr<TableFlavor> tableFlavor_;
        std::unique_ptr<Places> places_;
    };
}