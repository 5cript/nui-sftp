#pragma once

#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/flavor.hpp>

#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/api/keyboard_event.hpp>

#include <vector>
#include <set>
#include <memory>
#include <optional>
#include <functional>

namespace NuiFileExplorer
{
    /// Manages item selection for both icon and table flavors.
    ///
    /// Two-layer model
    /// ---------------
    ///   Layer 1 — Range layer  (anchor_ + flag_)
    ///     The contiguous shift-selection.  Shift+click and Shift+Arrow only ever
    ///     touch this layer.  A plain click sets anchor_ and clears flag_ (single-
    ///     item range).  Everything between anchor_ and flag_ (inclusive, min/max
    ///     order) is "in range".
    ///
    ///   Layer 2 — Ctrl mask  (ctrlAdd_ + ctrlRemove_)
    ///     An additive/subtractive mask applied on top of the range.
    ///     ctrlAdd_    – items that are selected regardless of the range.
    ///     ctrlRemove_ – items that are punched out of the range.
    ///     Ctrl+click dispatches to one of four cases (see onItemClicked).
    ///     This layer is NEVER touched by shift or plain-arrow operations.
    ///
    ///   Visible selection = (range(anchor_, flag_) − ctrlRemove_) ∪ ctrlAdd_
    ///
    /// Box-select (drag rectangle in icon flavor)
    /// ------------------------------------------
    ///   Plain drag   → clears both ctrl sets, sets anchor_/flag_ to cover the
    ///                  bounding-box items.  Call selectRange(begin, end).
    ///   Ctrl+drag    → leaves anchor_/flag_ alone, inserts intersecting items
    ///                  into ctrlAdd_.  Call ctrlAddRange(begin, end).
    ///
    /// Keyboard navigation — no wrapping, clamped at boundaries
    /// ---------------------------------------------------------
    ///   Arrow (no modifier)  → clear both ctrl sets, clear flag_, move anchor_
    ///                          one step; clamps at 0 / last item.
    ///   Shift+Arrow          → move flag_ one step; clamps at 0 / last item.
    ///                          Special boundary fill:
    ///                            Shift+Down at last row → extend flag_ to the
    ///                              last item (rightward fill to end of row).
    ///                            Shift+Up at row 0 → extend flag_ to index 0
    ///                              (leftward fill to start of row).
    ///                          Both ctrl sets are untouched.
    ///   Home / End           → collapse to first selectable / last item;
    ///                          clear both ctrl sets, clear flag_.
    ///   Shift+Home / Shift+End → extend flag_ to 0 / last item.
    ///   Ctrl+A               → select all (clear ctrl sets, anchor_=0,
    ///                          flag_=last).
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
        /// Register a callback that scrolls the item at the given flat index into
        /// view.  Called automatically after every user-driven selection change
        /// (click, keyboard) with the "active index": flag_ if live, else anchor_.
        /// Not called by selectAll / deselectAll / selectRange / ctrlAddRange.
        void setScrollIntoViewCallback(std::function<void(std::size_t)> callback);

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
        /// into view. Used by type-ahead.
        void jumpTo(std::size_t index);

        /// Ctrl+box-drag: add items into ctrlAdd_ without touching the range layer.
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

        /// Rebuild visual isSelected() for every item from the two-layer model.
        /// selected[i] = (inRange(i) && !ctrlRemove_.count(i)) || ctrlAdd_.count(i)
        void rebuildSelection();

        /// Whether index i falls inside [min(anchor_,flag_), max(anchor_,flag_)].
        bool inRange(std::size_t i) const;

        static void rangeMinMax(std::size_t a, std::size_t b, std::size_t& lo, std::size_t& hi);

        std::optional<std::size_t> itemIndex(ItemWithInternals const& item) const;
        std::optional<std::size_t> itemIndex(std::filesystem::path const& path) const;

        bool isSelectablePath(std::size_t idx) const;

        /// Apply one navigation step to 'from' according to the event key.
        std::size_t navigate(std::size_t from, Nui::WebApi::KeyboardEvent const& event) const;

        /// Returns flag_ if active, else anchor_.  nullopt if nothing is selected.
        std::optional<std::size_t> activeIndex() const;

        /// Fires scrollIntoView_ for the current activeIndex if it differs from
        /// lastScrolledIndex_ and scrollIntoView_ is set.
        void maybeScrollActiveIntoView();

      private:
        Nui::Observed<std::vector<ItemWithInternals>>* items_;

        // Layer 1 — range
        std::optional<std::size_t> anchor_; ///< Fixed end of the shift-range / last plain click.
        std::optional<std::size_t> flag_; ///< Moving end; nullopt = single-item range at anchor_.

        // Layer 2 — ctrl mask
        std::set<std::size_t> ctrlAdd_; ///< Selected regardless of the range.
        std::set<std::size_t> ctrlRemove_; ///< Punched out of the range.

        // Grid layout (updated by setGrid / setFlavor)
        Flavor currentFlavor_;
        std::size_t gridColumns_{1};
        std::size_t gridRows_{1};

        // Step size used by PageUp / PageDown. 0 means "fall back to visible rows * columns".
        std::size_t pageJumpSize_{0};

        // Scroll-into-view
        std::function<void(std::size_t)> scrollIntoView_{};
        std::optional<std::size_t> lastScrolledIndex_{};
    };
}