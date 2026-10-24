#pragma once

#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/flavor.hpp>
#include <nui-file-explorer/side/selection_model.hpp>

#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/api/keyboard_event.hpp>

#include <vector>
#include <set>
#include <memory>
#include <optional>
#include <functional>

namespace NuiFileExplorer
{
    /**
     * @brief Selection of the items of one side for both flavors, the model is a @ref SelectionModel.
     *
     * Shift+click and Shift+Arrow move the flag, a plain click or arrow collapses to one item, Ctrl+click flips one
     * item through the mask. Box select: a plain drag replaces the selection (@ref selectRange), a Ctrl+drag adds to
     * it (@ref ctrlAddRange). Navigation clamps at the first and last item, Shift+Down on the last row and Shift+Up
     * on the first row extend to the last and first item. Home lands on the first selectable item, Ctrl+A selects
     * every selectable item.
     */
    class SelectionManager
    {
      public:
        SelectionManager(Nui::Observed<std::vector<ItemWithInternals>>& items, Flavor flavor);

        // ------------------------------------------------------------------ basic ops
        void loseTrackToAllSelections(); ///< Forget tracking without touching DOM state.
        void deselectAll();
        void selectAll();
        void select(std::size_t index);
        void deselect(std::size_t index);
        void toggle(std::size_t index);
        void select(ItemWithInternals const& item);
        void select(std::filesystem::path const& path);
        void deselect(ItemWithInternals const& item);
        void toggle(ItemWithInternals const& item);

        // ------------------------------------------------------------------ queries
        std::size_t selectedCount() const;
        bool isSelected(std::size_t index) const;
        bool isAnySelected() const;
        std::vector<Item> selectedItems() const;
        std::set<std::filesystem::path> selectedPaths() const;

        // ------------------------------------------------------------------ layout
        void setGrid(std::size_t columns, std::size_t rows);
        void setFlavor(Flavor flavor);

        /**
         *  @brief Configure how many items one PageUp/PageDown press advances through.
         *  @param size 0 resets the manager to its default (visible rows * columns).
         */
        void setPageJumpSize(std::size_t size);

        // ------------------------------------------------------------------ scroll
        /**
         * @brief Registers a callback that scrolls the item at a flat index into view. Called after every user
         *        driven selection change (click, keyboard) with the active index of the model, not by selectAll,
         *        deselectAll, selectRange or ctrlAddRange.
         */
        void setScrollIntoViewCallback(std::function<void(std::size_t)> callback);

        /**
         * @brief Items for which @p isVisible is false (hidden by a filter) are never selected, not by select-all and
         *        not inside a shift range. Call @ref refreshSelection when the answer changes.
         */
        void setVisibilityPredicate(std::function<bool(std::size_t)> isVisible);

        /**
         * @brief Keeps the items selected now as an explicit selection and drops the ones that are no longer
         *        visible. Call it after the visibility changed, so items hidden while a range or select-all was made
         *        never become selected when they show again.
         */
        void refreshSelection();

        // ------------------------------------------------------------------ interaction
        void onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent const& event);

        /// Returns true if the keyboard event was consumed.
        bool onKeyboardEvent(Nui::WebApi::KeyboardEvent const& event);

        // ------------------------------------------------------------------ range helpers (public for icon_flavor
        // drag-box)

        /// Plain box-drag: replace range layer, clear ctrl mask.
        void selectRange(std::size_t begin, std::size_t endInclusive);

        /// Jump the cursor to @p index as if it were a plain click: clears the ctrl mask,
        /// collapses the range to a single-item selection at @p index, and scrolls it
        /// into view, also when it was the last item scrolled to. Used by type-ahead.
        void jumpTo(std::size_t index);

        /**
         * @brief Jumps to the selectable item at @p path as @ref jumpTo(std::size_t) does.
         *
         * @return Whether there is such an item.
         */
        bool jumpTo(std::filesystem::path const& path);

        /**
         * @brief Ctrl+box-drag: adds the selectable items to the mask without touching the range.
         */
        void ctrlAddRange(std::size_t begin, std::size_t endInclusive);

      private:
        // ============================================================= Grid helper
        /// Abstracts an M×N grid whose last row may be incomplete.
        /// All navigation is flat-index based.  Steps clamp — no wrapping.
        ///
        /// ArrowDown/Up boundary fill
        /// --------------------------
        ///   stepDown when already on the last row:
        ///     Returns itemCount_-1, filling selection to the end of the partial row.
        ///   stepUp when already on row 0:
        ///     Returns 0, filling selection to the start of the row.
        ///
        /// Table flavor: columns_ is treated as 1; Down/Up are ±1 clamped;
        /// Right/Left are aliases for Down/Up.
        class Grid
        {
          public:
            Grid() = default;
            Grid(std::size_t columns, std::size_t rows, std::size_t itemCount, Flavor flavor)
                : columns_{std::max(std::size_t{1}, columns)}
                , rows_{std::max(std::size_t{1}, rows)}
                , itemCount_{itemCount}
                , flavor_{flavor}
            {}

            std::size_t columns() const
            {
                return columns_;
            }
            std::size_t rows() const
            {
                return rows_;
            }
            std::size_t items() const
            {
                return itemCount_;
            }

            /// Number of filled cells in the last row (1 … columns_).
            std::size_t itemsInLastRow() const
            {
                if (itemCount_ == 0)
                    return 0;
                const auto r = itemCount_ % columns_;
                return r == 0 ? columns_ : r;
            }

            bool isTableFlavor() const
            {
                return flavor_ == Flavor::Table;
            }

            /// Move one step right; clamps at last item.
            std::size_t stepRight(std::size_t idx) const
            {
                if (itemCount_ == 0)
                    return 0;
                if (isTableFlavor())
                    return stepDown(idx);
                return std::min(idx + 1, itemCount_ - 1);
            }

            /// Move one step left; clamps at 0.
            std::size_t stepLeft(std::size_t idx) const
            {
                if (itemCount_ == 0)
                    return 0;
                if (isTableFlavor())
                    return stepUp(idx);
                return idx > 0 ? idx - 1 : 0;
            }

            /// Move one step down (next row, same column).
            /// Boundary fill: if already on the last row, return itemCount_-1.
            std::size_t stepDown(std::size_t idx) const
            {
                if (itemCount_ == 0)
                    return 0;
                if (isTableFlavor())
                    return std::min(idx + 1, itemCount_ - 1);

                const auto next = idx + columns_;
                if (next < itemCount_)
                    return next;

                // On the last row — fill rightward to the last item.
                return itemCount_ - 1;
            }

            /// Move one step up (previous row, same column).
            /// Boundary fill: if already on row 0, return 0.
            std::size_t stepUp(std::size_t idx) const
            {
                if (itemCount_ == 0)
                    return 0;
                if (isTableFlavor())
                    return idx > 0 ? idx - 1 : 0;

                if (idx >= columns_)
                    return idx - columns_;

                // On row 0 — fill leftward to index 0.
                return 0;
            }

          private:
            std::size_t columns_{1};
            std::size_t rows_{1};
            std::size_t itemCount_{0};
            Flavor flavor_{Flavor::Table};
        };

        // ============================================================= helpers
        Grid makeGrid() const;

        /**
         * @brief Sets isSelected of every item from the model, only selectable items can be selected.
         */
        void rebuildSelection();

        std::optional<std::size_t> itemIndex(ItemWithInternals const& item) const;
        std::optional<std::size_t> itemIndex(std::filesystem::path const& path) const;

        bool isSelectablePath(std::size_t idx) const;

        /// Apply one navigation step to 'from' according to the event key.
        std::size_t navigate(std::size_t from, Nui::WebApi::KeyboardEvent const& event) const;

        /**
         * @brief Scrolls the active index of the model into view when it changed since the last scroll.
         */
        void maybeScrollActiveIntoView();

      private:
        Nui::Observed<std::vector<ItemWithInternals>>* items_;

        SelectionModel selection_{};

        // Grid layout (updated by setGrid / setFlavor)
        Flavor currentFlavor_;
        std::size_t gridColumns_{1};
        std::size_t gridRows_{1};

        // Step size used by PageUp / PageDown. 0 means "fall back to visible rows * columns".
        std::size_t pageJumpSize_{0};

        // Scroll-into-view
        std::function<void(std::size_t)> scrollIntoView_{};
        std::function<bool(std::size_t)> isVisible_{};
        std::optional<std::size_t> lastScrolledIndex_{};
    };
}