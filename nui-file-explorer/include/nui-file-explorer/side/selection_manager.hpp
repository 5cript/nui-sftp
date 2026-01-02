#pragma once

#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/flavor.hpp>

#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/api/keyboard_event.hpp>

#include <vector>
#include <memory>
#include <optional>

namespace NuiFileExplorer
{
    class SelectionManager
    {
      public:
        constexpr static std::size_t maxSelectableSearchAttempts = 5;

        SelectionManager(Nui::Observed<std::vector<ItemWithInternals>>& items, Flavor flavor);

        /// Does not iterate the items to deselect all.
        void loseTrackToAllSelections();

        void deselectAll();
        void selectAll();
        bool select(std::size_t index);
        void deselect(std::size_t index);
        void toggle(std::size_t index);
        void select(ItemWithInternals const& item);
        void deselect(ItemWithInternals const& item);
        void toggle(ItemWithInternals const& item);
        std::size_t selectedCount() const;
        bool isSelected(std::size_t index) const;
        bool isAnySelected() const;
        void setGrid(std::size_t width, std::size_t height);
        void setFlavor(Flavor flavor);

        void onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent const& event);

        /// Returns true if the event was consumed.
        bool onKeyboardEvent(Nui::WebApi::KeyboardEvent const& event);

        void selectRange(std::size_t begin, std::size_t endInclusive);

        std::vector<Item> selectedItems() const;

      private:
        class GridPosition
        {
          public:
            using CoordinateType = std::make_signed<std::size_t>::type;

            GridPosition(SelectionManager const& manager, std::size_t index)
                : manager_{&manager}
                , row_{manager_->currentFlavor_ == Flavor::Icons ? static_cast<CoordinateType>(index / std::max(std::size_t{1}, manager_->gridColumns_)) : static_cast<CoordinateType>(index)}
                , col_{
                      manager_->currentFlavor_ == Flavor::Icons
                          ? static_cast<CoordinateType>(index % std::max(std::size_t{1}, manager_->gridColumns_))
                          : 0
                  }
            {}

            GridPosition(SelectionManager const& manager, CoordinateType row, CoordinateType col)
                : manager_{&manager}
                , row_{row}
                , col_{col}
            {}

            bool operator==(GridPosition const& other) const
            {
                return normalRow() == other.normalRow() && normalCol() == other.normalCol();
            }
            std::size_t toIndex() const
            {
                return static_cast<std::size_t>(
                    normalized().row_ * static_cast<CoordinateType>(manager_->gridColumns_) + normalized().col_
                );
            }
            /// Returns whether or not there is a valid item at this position.
            bool isValid() const
            {
                const auto norm = normalized();
                return norm.row_ >= 0 && norm.col_ >= 0 && static_cast<std::size_t>(norm.row_) < manager_->gridRows_ &&
                    static_cast<std::size_t>(norm.col_) < manager_->gridColumns_;
            }
            /// Gird positions can become virtual by having negative coordinates or coordinates beyond the grid size.
            GridPosition normalized() const
            {
                return GridPosition{*manager_, normalRow(), normalCol()};
            }
            CoordinateType normalRow() const
            {
                const auto rows = static_cast<CoordinateType>(manager_->gridRows_);
                if (rows <= 0)
                    return row_;

                auto row = row_;

                row %= rows;
                if (row < 0)
                    row += rows;
                return row;
            }
            CoordinateType normalCol() const
            {
                const auto cols = static_cast<CoordinateType>(manager_->gridColumns_);
                if (cols < 0)
                    return col_;

                auto col = col_;

                col %= cols;
                if (col < 0)
                    col += cols;
                return col;
            }
            void up()
            {
                if (static_cast<CoordinateType>(manager_->gridRows_) <= 1)
                    return;

                --row_;

                // check if were are in the last row now, and outside the regular items range, if so go up one more.
                if (normalRow() == static_cast<CoordinateType>(manager_->gridRows_) - 1 &&
                    normalCol() >= static_cast<CoordinateType>(manager_->itemsInLastRow()))
                    --row_;
            }
            void down()
            {
                if (static_cast<CoordinateType>(manager_->gridRows_) <= 1)
                    return;

                ++row_;

                // check if were are in the last row now, and outside the regular items range, if so go down one more.
                if (normalRow() == static_cast<CoordinateType>(manager_->gridRows_) - 1 &&
                    normalCol() >= static_cast<CoordinateType>(manager_->itemsInLastRow()))
                    ++row_;
            }
            bool isUnselected() const
            {
                if (!isValid())
                    return false;
                return manager_->isIndexUnselected(toIndex());
            }
            bool isSelected() const
            {
                if (!isValid())
                    return false;
                return manager_->isSelected(toIndex());
            }
            bool isWrapped() const
            {
                return row_ < 0 || col_ < 0;
            }

          private:
            SelectionManager const* manager_;
            CoordinateType row_;
            CoordinateType col_;
        };

        std::optional<std::size_t> findFirstSelectable() const;
        std::optional<std::size_t> findLastSelectable() const;
        std::optional<std::size_t> findNextSelectable(std::size_t index) const;
        std::optional<std::size_t> findPreviousSelectable(std::size_t index) const;
        bool isIndexUnselected(std::make_signed_t<std::size_t> index) const;
        bool isIndexUnselected(std::size_t index) const;
        GridPosition calculateGridPositionFromIndex(std::size_t index) const;
        std::size_t itemsInLastRow() const;

      private:
        Nui::Observed<std::vector<ItemWithInternals>>* items_;
        std::set<std::size_t> selectedIndices_;
        std::optional<std::size_t> currentSelectionStart_{std::nullopt};
        Flavor currentFlavor_;
        std::size_t gridColumns_{1};
        std::size_t gridRows_{1};
    };
}