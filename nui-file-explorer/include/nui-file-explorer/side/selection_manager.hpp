#pragma once

#include <nui-file-explorer/item_with_internals.hpp>

#include <nui/frontend/api/mouse_event.hpp>

#include <vector>
#include <memory>
#include <optional>

namespace NuiFileExplorer
{
    class SelectionManager
    {
      public:
        explicit SelectionManager(Nui::Observed<std::vector<ItemWithInternals>>& items);

        /// Does not iterate the items to deselect all.
        void loseTrackToAllSelections();

        void deselectAll();
        void selectAll();
        void select(std::size_t index);
        void deselect(std::size_t index);
        void toggle(std::size_t index);
        void select(ItemWithInternals const& item);
        void deselect(ItemWithInternals const& item);
        void toggle(ItemWithInternals const& item);
        std::size_t selectedCount() const;
        bool isSelected(std::size_t index) const;
        bool isAnySelected() const;
        void setGrid(std::size_t width, std::size_t height);

        void onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent const& event);

      private:
        Nui::Observed<std::vector<ItemWithInternals>>* items_;
        std::set<std::size_t> selectedIndices_;
        std::optional<std::size_t> currentSelectionStart_{std::nullopt};
        std::size_t gridWidth_{1};
        std::size_t gridHeight_{1};
    };
}