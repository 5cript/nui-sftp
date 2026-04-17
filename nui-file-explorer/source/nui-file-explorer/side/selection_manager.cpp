#include <nui-file-explorer/side/selection_manager.hpp>

#include <algorithm>

namespace NuiFileExplorer
{
    // =========================================================================
    // Scroll-into-view
    // =========================================================================

    void SelectionManager::setScrollIntoViewCallback(std::function<void(std::size_t)> callback)
    {
        scrollIntoView_ = std::move(callback);
    }

    std::optional<std::size_t> SelectionManager::activeIndex() const
    {
        if (flag_.has_value())
            return flag_;
        if (anchor_.has_value())
            return anchor_;
        return std::nullopt;
    }

    void SelectionManager::maybeScrollActiveIntoView()
    {
        if (!scrollIntoView_)
            return;
        const auto idx = activeIndex();
        if (!idx.has_value())
            return;
        if (lastScrolledIndex_ == idx)
            return;
        lastScrolledIndex_ = idx;
        scrollIntoView_(*idx);
    }

    // =========================================================================
    // Construction
    // =========================================================================

    SelectionManager::SelectionManager(Nui::Observed<std::vector<ItemWithInternals>>& items, Flavor flavor)
        : items_{&items}
        , currentFlavor_{flavor}
    {}

    // =========================================================================
    // Layout
    // =========================================================================

    void SelectionManager::setGrid(std::size_t columns, std::size_t rows)
    {
        gridColumns_ = std::max(std::size_t{1}, columns);
        gridRows_ = std::max(std::size_t{1}, rows);
    }

    void SelectionManager::setFlavor(Flavor flavor)
    {
        currentFlavor_ = flavor;
    }

    void SelectionManager::setPageJumpSize(std::size_t size)
    {
        pageJumpSize_ = size;
    }

    // =========================================================================
    // Internal helpers
    // =========================================================================

    SelectionManager::Grid SelectionManager::makeGrid() const
    {
        return Grid{gridColumns_, gridRows_, items_->value().size(), currentFlavor_};
    }

    void SelectionManager::rangeMinMax(std::size_t a, std::size_t b, std::size_t& lo, std::size_t& hi)
    {
        if (a <= b)
        {
            lo = a;
            hi = b;
        }
        else
        {
            lo = b;
            hi = a;
        }
    }

    bool SelectionManager::inRange(std::size_t i) const
    {
        if (!anchor_.has_value())
            return false;

        std::size_t lo, hi;
        if (flag_.has_value())
            rangeMinMax(*anchor_, *flag_, lo, hi);
        else
            lo = hi = *anchor_;

        return i >= lo && i <= hi;
    }

    std::optional<std::size_t> SelectionManager::itemIndex(ItemWithInternals const& item) const
    {
        const auto& vec = items_->value();
        for (std::size_t i = 0; i < vec.size(); ++i)
            if (&vec[i] == &item)
                return i;
        return std::nullopt;
    }

    std::optional<std::size_t> SelectionManager::itemIndex(std::filesystem::path const& path) const
    {
        const auto& vec = items_->value();
        for (std::size_t i = 0; i < vec.size(); ++i)
            if (vec[i].item.path == path)
                return i;
        return std::nullopt;
    }

    bool SelectionManager::isSelectablePath(std::size_t idx) const
    {
        const auto& vec = items_->value();
        if (idx >= vec.size())
            return false;
        return vec[idx].item.path.filename() != "..";
    }

    // =========================================================================
    // rebuildSelection
    // =========================================================================

    void SelectionManager::rebuildSelection()
    {
        auto& vec = items_->value();
        for (std::size_t i = 0; i < vec.size(); ++i)
        {
            const bool selected = isSelectablePath(i) &&
                ((inRange(i) && ctrlRemove_.count(i) == 0) || ctrlAdd_.count(i) > 0);
            vec[i].isSelected(selected);
        }
        Nui::globalEventContext.sync();
    }

    // =========================================================================
    // Basic ops
    // =========================================================================

    void SelectionManager::loseTrackToAllSelections()
    {
        anchor_.reset();
        flag_.reset();
        ctrlAdd_.clear();
        ctrlRemove_.clear();
    }

    void SelectionManager::deselectAll()
    {
        loseTrackToAllSelections();
        for (auto& item : items_->value())
            item.isSelected(false);
        Nui::globalEventContext.sync();
    }

    void SelectionManager::selectAll()
    {
        ctrlAdd_.clear();
        ctrlRemove_.clear();
        flag_.reset();
        const auto n = items_->value().size();
        if (n > 0)
        {
            anchor_ = 0;
            flag_ = n - 1;
        }
        else
        {
            anchor_.reset();
        }
        rebuildSelection();
    }

    // select/deselect/toggle operate on ctrlAdd_ so they compose cleanly with
    // any existing range layer.

    void SelectionManager::select(std::size_t index)
    {
        if (!isSelectablePath(index))
            return;
        ctrlRemove_.erase(index); // un-punch if it was punched out
        ctrlAdd_.insert(index);
        rebuildSelection();
    }

    void SelectionManager::deselect(std::size_t index)
    {
        ctrlAdd_.erase(index);
        if (inRange(index))
            ctrlRemove_.insert(index); // punch it out of the range
        rebuildSelection();
    }

    void SelectionManager::toggle(std::size_t index)
    {
        if (isSelected(index))
            deselect(index);
        else
            select(index);
    }

    void SelectionManager::select(ItemWithInternals const& item)
    {
        if (auto idx = itemIndex(item); idx.has_value())
            select(*idx);
    }

    void SelectionManager::select(std::filesystem::path const& path)
    {
        if (auto idx = itemIndex(path); idx.has_value())
            select(*idx);
    }

    void SelectionManager::deselect(ItemWithInternals const& item)
    {
        if (auto idx = itemIndex(item); idx.has_value())
            deselect(*idx);
    }

    void SelectionManager::toggle(ItemWithInternals const& item)
    {
        if (auto idx = itemIndex(item); idx.has_value())
            toggle(*idx);
    }

    // =========================================================================
    // Range helpers (drag-box)
    // =========================================================================

    void SelectionManager::selectRange(std::size_t begin, std::size_t endInclusive)
    {
        // Plain drag: replace both layers entirely.
        ctrlAdd_.clear();
        ctrlRemove_.clear();
        anchor_ = begin;
        flag_ = endInclusive;
        rebuildSelection();
    }

    void SelectionManager::jumpTo(std::size_t index)
    {
        if (index >= items_->value().size())
            return;
        ctrlAdd_.clear();
        ctrlRemove_.clear();
        flag_.reset();
        anchor_ = index;
        rebuildSelection();
        maybeScrollActiveIntoView();
    }

    void SelectionManager::ctrlAddRange(std::size_t begin, std::size_t endInclusive)
    {
        // Ctrl+drag: add items to ctrlAdd_, don't touch the range layer.
        std::size_t lo, hi;
        rangeMinMax(begin, endInclusive, lo, hi);
        for (std::size_t i = lo; i <= hi; ++i)
        {
            if (!isSelectablePath(i))
                continue;
            ctrlRemove_.erase(i); // restore any previously punched-out item
            ctrlAdd_.insert(i);
        }
        rebuildSelection();
    }

    // =========================================================================
    // Queries
    // =========================================================================

    std::size_t SelectionManager::selectedCount() const
    {
        std::size_t count = 0;
        for (auto const& item : items_->value())
            if (item.isSelected())
                ++count;
        return count;
    }

    bool SelectionManager::isSelected(std::size_t index) const
    {
        const auto& vec = items_->value();
        if (index >= vec.size())
            return false;
        return vec[index].isSelected();
    }

    bool SelectionManager::isAnySelected() const
    {
        for (auto const& item : items_->value())
            if (item.isSelected())
                return true;
        return false;
    }

    std::vector<Item> SelectionManager::selectedItems() const
    {
        std::vector<Item> result;
        for (auto const& item : items_->value())
            if (item.isSelected())
                result.push_back(item.item);
        return result;
    }

    std::set<std::filesystem::path> SelectionManager::selectedPaths() const
    {
        std::set<std::filesystem::path> result;
        for (auto const& item : items_->value())
            if (item.isSelected())
                result.insert(item.item.path);
        return result;
    }

    // =========================================================================
    // Mouse interaction
    // =========================================================================

    void SelectionManager::onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent const& event)
    {
        const auto idxOpt = itemIndex(item);
        if (!idxOpt.has_value())
            return;
        const std::size_t idx = *idxOpt;

        if (!isSelectablePath(idx))
        {
            // Non-selectable items (e.g. "..") can never be selected, but must
            // always be deselectable in case they somehow ended up selected.
            if (isSelected(idx))
                deselect(idx);
            return;
        }

        const bool ctrl = event.ctrlKey();
        const bool shift = event.shiftKey();

        if (shift && !ctrl)
        {
            // ---- Shift+click: move flag, keep anchor, leave ctrl mask alone ----
            if (!anchor_.has_value())
                anchor_ = idx;
            flag_ = idx;
            rebuildSelection();
            maybeScrollActiveIntoView();
        }
        else if (ctrl && !shift)
        {
            // ---- Ctrl+click: toggle via the ctrl mask; never touch anchor/flag ----
            //
            // Four cases:
            //   1. In ctrlRemove_ (punched out of range) → restore to range:
            //      remove from ctrlRemove_.
            //   2. In ctrlAdd_ (explicitly added) → remove from ctrlAdd_.
            //   3. In range (selected via range, not punched) → punch out:
            //      insert into ctrlRemove_.
            //   4. Not selected at all → add to ctrlAdd_.
            //
            // After this, anchor_/flag_ are completely unchanged.
            if (ctrlRemove_.count(idx))
            {
                // Case 1: was punched out — restore it.
                ctrlRemove_.erase(idx);
            }
            else if (ctrlAdd_.count(idx))
            {
                // Case 2: was explicitly added — remove it.
                ctrlAdd_.erase(idx);
            }
            else if (inRange(idx))
            {
                // Case 3: selected via range — punch it out.
                ctrlRemove_.insert(idx);
            }
            else
            {
                // Case 4: not selected — add it.
                ctrlAdd_.insert(idx);
            }
            rebuildSelection();
            // Ctrl+click does not move anchor/flag, so we scroll to the clicked
            // item directly — it is the one the user just interacted with.
            lastScrolledIndex_ = idx;
            if (scrollIntoView_)
                scrollIntoView_(idx);
        }
        else if (ctrl && shift)
        {
            // ---- Ctrl+Shift+click: extend the range but keep the ctrl mask ----
            if (!anchor_.has_value())
                anchor_ = idx;
            flag_ = idx;
            rebuildSelection();
            maybeScrollActiveIntoView();
        }
        else
        {
            // ---- Plain click: reset everything, select only this item ----
            ctrlAdd_.clear();
            ctrlRemove_.clear();
            flag_.reset();
            anchor_ = idx;
            rebuildSelection();
            maybeScrollActiveIntoView();
        }
    }

    // =========================================================================
    // Keyboard navigation
    // =========================================================================

    std::size_t SelectionManager::navigate(std::size_t from, Nui::WebApi::KeyboardEvent const& event) const
    {
        const auto grid = makeGrid();
        const auto key = event.key();

        if (key == "ArrowRight")
            return grid.stepRight(from);
        if (key == "ArrowLeft")
            return grid.stepLeft(from);
        if (key == "ArrowDown")
            return grid.stepDown(from);
        if (key == "ArrowUp")
            return grid.stepUp(from);

        return from;
    }

    bool SelectionManager::onKeyboardEvent(Nui::WebApi::KeyboardEvent const& event)
    {
        const auto key = event.key();

        const bool isArrow = (key == "ArrowRight" || key == "ArrowLeft" || key == "ArrowDown" || key == "ArrowUp");
        const bool isHome = (key == "Home");
        const bool isEnd = (key == "End");
        const bool isPageDown = (key == "PageDown");
        const bool isPageUp = (key == "PageUp");
        const bool isSelectAll = (key == "a" || key == "A") && event.ctrlKey();

        if (!isArrow && !isHome && !isEnd && !isPageDown && !isPageUp && !isSelectAll)
            return false;

        if (isSelectAll)
        {
            selectAll();
            return true;
        }

        const bool shift = event.shiftKey();
        const auto n = items_->value().size();

        if (isPageDown || isPageUp)
        {
            if (n == 0)
                return true;

            const std::size_t step = pageJumpSize_ > 0 ? pageJumpSize_ : std::max(std::size_t{1}, gridColumns_ * gridRows_);
            const std::size_t lastIdx = n - 1;
            const auto jump = [&](std::size_t from) -> std::size_t {
                if (isPageDown)
                    return from + step >= n ? lastIdx : from + step;
                return from > step ? from - step : 0;
            };

            if (shift)
            {
                if (!flag_.has_value())
                    flag_ = anchor_.value_or(0);
                flag_ = jump(*flag_);
                rebuildSelection();
                maybeScrollActiveIntoView();
            }
            else
            {
                ctrlAdd_.clear();
                ctrlRemove_.clear();
                flag_.reset();
                anchor_ = jump(anchor_.value_or(0));
                rebuildSelection();
                maybeScrollActiveIntoView();
            }
            return true;
        }

        if (isHome || isEnd)
        {
            if (n == 0)
                return true;

            if (shift)
            {
                // Extend flag_ to the boundary; anchor and ctrl mask untouched.
                if (!flag_.has_value())
                    flag_ = anchor_.value_or(0);
                flag_ = isEnd ? (n - 1) : 0;
                rebuildSelection();
                maybeScrollActiveIntoView();
            }
            else
            {
                // Plain Home/End: collapse to a single item at the boundary.
                ctrlAdd_.clear();
                ctrlRemove_.clear();
                flag_.reset();
                if (isEnd)
                {
                    anchor_ = n - 1;
                }
                else
                {
                    // Home: land on the first selectable item (skip "..").
                    anchor_ = 0;
                    while (*anchor_ < n && !isSelectablePath(*anchor_))
                        anchor_ = *anchor_ + 1;
                    if (*anchor_ >= n)
                        anchor_ = 0;
                }
                rebuildSelection();
                maybeScrollActiveIntoView();
            }
            return true;
        }

        if (shift)
        {
            // Move flag; anchor and ctrl mask are untouched.
            // If no range is active yet, start the flag from the anchor (or 0).
            if (!flag_.has_value())
                flag_ = anchor_.value_or(0);

            flag_ = navigate(*flag_, event);
            rebuildSelection();
            maybeScrollActiveIntoView();
        }
        else
        {
            // Plain arrow: drop the ctrl mask entirely and collapse to a single item.
            ctrlAdd_.clear();
            ctrlRemove_.clear();
            flag_.reset();

            const std::size_t next = navigate(anchor_.value_or(0), event);
            anchor_ = next;
            rebuildSelection();
            maybeScrollActiveIntoView();
        }

        return true;
    }
}