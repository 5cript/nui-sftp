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

    void SelectionManager::maybeScrollActiveIntoView()
    {
        if (!scrollIntoView_)
            return;
        const auto idx = selection_.activeIndex();
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
            if (vec[i].item.fullPath == path)
                return i;
        return std::nullopt;
    }

    bool SelectionManager::isSelectablePath(std::size_t idx) const
    {
        const auto& vec = items_->value();
        if (idx >= vec.size())
            return false;
        return !vec[idx].item.isParentLink && (!isVisible_ || isVisible_(idx));
    }

    void SelectionManager::setVisibilityPredicate(std::function<bool(std::size_t)> isVisible)
    {
        isVisible_ = std::move(isVisible);
    }

    void SelectionManager::refreshSelection()
    {
        const auto& vec = items_->value();
        std::set<std::size_t> shown;
        for (std::size_t index = 0; index < vec.size(); ++index)
            if (vec[index].isSelected())
                shown.insert(index);
        selection_.pin(std::move(shown));
        rebuildSelection();
    }

    // =========================================================================
    // rebuildSelection
    // =========================================================================

    void SelectionManager::rebuildSelection()
    {
        auto& vec = items_->value();
        for (std::size_t i = 0; i < vec.size(); ++i)
        {
            vec[i].isSelected(isSelectablePath(i) && selection_.contains(i));
        }
        Nui::globalEventContext.sync();
    }

    // =========================================================================
    // Basic ops
    // =========================================================================

    void SelectionManager::loseTrackToAllSelections()
    {
        selection_.clear();
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
        selection_.selectAll(items_->value().size());
        rebuildSelection();
    }

    void SelectionManager::select(std::size_t index)
    {
        if (!isSelectablePath(index))
            return;
        selection_.add(index);
        rebuildSelection();
    }

    void SelectionManager::deselect(std::size_t index)
    {
        selection_.remove(index);
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
        selection_.replaceRange(begin, endInclusive);
        rebuildSelection();
    }

    void SelectionManager::jumpTo(std::size_t index)
    {
        if (index >= items_->value().size())
            return;
        selection_.collapseTo(index);
        rebuildSelection();
        lastScrolledIndex_ = index;
        if (scrollIntoView_)
            scrollIntoView_(index);
    }

    bool SelectionManager::jumpTo(std::filesystem::path const& path)
    {
        const auto index = itemIndex(path);
        if (!index || !isSelectablePath(*index))
            return false;
        jumpTo(*index);
        return true;
    }

    void SelectionManager::ctrlAddRange(std::size_t begin, std::size_t endInclusive)
    {
        for (std::size_t index = std::min(begin, endInclusive); index <= std::max(begin, endInclusive); ++index)
        {
            if (isSelectablePath(index))
                selection_.add(index);
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
                result.insert(item.item.fullPath);
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

        const bool ctrl = event.ctrlKey() || event.metaKey();
        const bool shift = event.shiftKey();

        if (ctrl && !shift)
        {
            selection_.toggleMasked(idx);
            rebuildSelection();
            // Ctrl+click moves neither end of the range, the clicked item is the one to show.
            lastScrolledIndex_ = idx;
            if (scrollIntoView_)
                scrollIntoView_(idx);
            return;
        }

        // Shift+click and Ctrl+Shift+click extend the range and keep the mask, a plain click selects only this item.
        if (shift)
            selection_.extendTo(idx);
        else
            selection_.collapseTo(idx);
        rebuildSelection();
        maybeScrollActiveIntoView();
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
        const bool isSelectAll = (key == "a" || key == "A") && (event.ctrlKey() || event.metaKey());

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
                selection_.extendTo(jump(selection_.activeIndex().value_or(0)));
            else
                selection_.collapseTo(jump(selection_.anchor().value_or(0)));
            rebuildSelection();
            maybeScrollActiveIntoView();
            return true;
        }

        if (isHome || isEnd)
        {
            if (n == 0)
                return true;

            if (shift)
                selection_.extendTo(isEnd ? n - 1 : 0);
            else if (isEnd)
                selection_.collapseTo(n - 1);
            else
            {
                // Home lands on the first selectable item, past "..".
                std::size_t first = 0;
                while (first < n && !isSelectablePath(first))
                    ++first;
                selection_.collapseTo(first < n ? first : 0);
            }
            rebuildSelection();
            maybeScrollActiveIntoView();
            return true;
        }

        if (shift)
            selection_.extendTo(navigate(selection_.activeIndex().value_or(0), event));
        else
            selection_.collapseTo(navigate(selection_.anchor().value_or(0), event));
        rebuildSelection();
        maybeScrollActiveIntoView();
        return true;
    }
}