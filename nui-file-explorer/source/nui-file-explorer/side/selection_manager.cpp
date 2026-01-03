#include <nui-file-explorer/side/selection_manager.hpp>

#include <nui/event_system/listen.hpp>
#include <nui/frontend/api/console.hpp>

namespace NuiFileExplorer
{
    SelectionManager::SelectionManager(Nui::Observed<std::vector<ItemWithInternals>>& items, Flavor flavor)
        : items_{&items}
        , currentFlavor_{flavor}
    {}

    void SelectionManager::loseTrackToAllSelections()
    {
        selectedIndices_.clear();
        currentSelectionStart_ = std::nullopt;
    }
    void SelectionManager::deselectAll()
    {
        for (auto index : selectedIndices_)
        {
            if (items_->value()[index].isSelected())
                *items_->value()[index].selected_ = false;
        }
        loseTrackToAllSelections();
    }
    void SelectionManager::selectAll()
    {
        // This makes sure that a new currentSelectionStart is assigned:
        deselectAll();
        for (std::size_t i = 0; i < items_->value().size(); ++i)
            select(i);
    }
    bool SelectionManager::select(std::size_t index)
    {
        if (index >= items_->value().size())
            return false;
        if (selectedIndices_.empty())
            currentSelectionStart_ = index;
        selectedIndices_.insert(index);
        *items_->value()[index].selected_ = true;
        return true;
    }
    void SelectionManager::select(std::filesystem::path const& path)
    {
        auto it = std::find_if(
            items_->value().begin(),
            items_->value().end(),
            [&path](auto const& item)
            {
                return item.item.path == path;
            }
        );
        if (it != items_->value().end())
        {
            const auto index = std::distance(items_->value().begin(), it);
            select(static_cast<std::size_t>(index));
        }
    }
    void SelectionManager::toggle(std::size_t index)
    {
        if (selectedIndices_.contains(index))
            deselect(index);
        else
            select(index);
    }
    void SelectionManager::select(ItemWithInternals const& item)
    {
        const auto index = item.indexDataAttribute();
        if (!index)
        {
            Nui::WebApi::Console::log("no data index on element");
            return;
        }
        select(static_cast<std::size_t>(*index));
    }
    void SelectionManager::deselect(std::size_t index)
    {
        if (index >= items_->value().size())
            return;
        selectedIndices_.erase(index);
        if (items_->value()[index].isSelected())
            *items_->value()[index].selected_ = false;
        if (selectedIndices_.empty())
            currentSelectionStart_ = std::nullopt;
    }
    void SelectionManager::deselect(ItemWithInternals const& item)
    {
        const auto index = item.indexDataAttribute();
        if (!index)
            return;
        deselect(static_cast<std::size_t>(*index));
    }
    void SelectionManager::toggle(ItemWithInternals const& item)
    {
        const auto index = item.indexDataAttribute();
        if (!index)
            return;
        toggle(static_cast<std::size_t>(*index));
    }
    std::size_t SelectionManager::selectedCount() const
    {
        return selectedIndices_.size();
    }
    bool SelectionManager::isSelected(std::size_t index) const
    {
        return selectedIndices_.contains(index);
    }
    bool SelectionManager::isAnySelected() const
    {
        return !selectedIndices_.empty();
    }
    void SelectionManager::setGrid(std::size_t width, std::size_t height)
    {
        gridColumns_ = width;
        gridRows_ = height;
    }
    void SelectionManager::setFlavor(Flavor flavor)
    {
        currentFlavor_ = flavor;
    }
    std::vector<Item> SelectionManager::selectedItems() const
    {
        std::vector<Item> result{};
        for (auto index : selectedIndices_)
        {
            if (items_->value()[index].isSelectable())
                result.push_back(items_->value()[index].item);
        }
        return result;
    }
    std::set<std::filesystem::path> SelectionManager::selectedPaths() const
    {
        std::set<std::filesystem::path> result{};
        for (auto index : selectedIndices_)
        {
            if (items_->value()[index].isSelectable())
                result.insert(items_->value()[index].item.path);
        }
        return result;
    }
    void SelectionManager::onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent const& event)
    {
        if (event.ctrlKey())
            return toggle(item);

        if (event.shiftKey())
        {
            auto self = items_->value().begin() + item.indexDataAttribute().value_or(0);

            // go backwards and forwards until we find another selected item:
            auto forwardSeeker = self + 1;
            auto backwardSeeker = self - 1;
            decltype(self) otherSelected = items_->value().end();

            if (self == items_->value().end())
                return;
            if (self == items_->value().begin())
                backwardSeeker = items_->value().begin();
            else if (self == items_->value().end() - 1)
                forwardSeeker = items_->value().end() - 1;

            while (forwardSeeker != items_->value().end() && backwardSeeker != items_->value().begin())
            {
                if (forwardSeeker->isSelected())
                {
                    otherSelected = forwardSeeker;
                    break;
                }
                if (backwardSeeker->isSelected())
                {
                    otherSelected = backwardSeeker;
                    break;
                }
                ++forwardSeeker;
                --backwardSeeker;
            }

            if (otherSelected == items_->value().end() && forwardSeeker != items_->value().end())
            {
                for (; forwardSeeker != items_->value().end(); ++forwardSeeker)
                {
                    if (forwardSeeker->isSelected())
                    {
                        otherSelected = forwardSeeker;
                        break;
                    }
                }
            }
            else if (otherSelected == items_->value().end() && backwardSeeker != items_->value().begin())
            {
                for (; backwardSeeker != items_->value().begin(); --backwardSeeker)
                {
                    if (backwardSeeker->isSelected())
                    {
                        otherSelected = backwardSeeker;
                        break;
                    }
                }
                if (backwardSeeker == items_->value().begin() && otherSelected == items_->value().end() &&
                    backwardSeeker->isSelected())
                    otherSelected = items_->value().begin();
            }

            deselectAll();
            if (otherSelected != items_->value().end())
            {
                auto start = std::min(self, otherSelected);
                auto end = std::max(self, otherSelected);
                auto offset = (end != items_->value().end()) ? 1 : 0;
                for (auto it = start; it != end + offset; ++it)
                {
                    select(*it);
                }
            }
            else
            {
                selectAll();
            }

            return;
        }

        deselectAll();
        select(item);
    }

    bool SelectionManager::onKeyboardEvent(Nui::WebApi::KeyboardEvent const& event)
    {
        if (event.key() == "a" && event.ctrlKey())
        {
            selectAll();
            return true;
        }
        if (event.key() == "Escape")
        {
            deselectAll();
            return true;
        }
        if (event.key() == "End")
        {
            auto lastSelectable = findLastSelectable();
            if (!lastSelectable)
            {
                return true; // event consumed, but nothing todo.
            }

            if (event.shiftKey())
            {
                selectRange(currentSelectionStart_.value_or(*lastSelectable), *lastSelectable);
                return true;
            }
            if (event.ctrlKey())
            {
                select(*lastSelectable);
                return true;
            }
            deselectAll();
            select(*lastSelectable);
            return true;
        }
        if (event.key() == "Home")
        {
            auto firstSelectable = findFirstSelectable();
            if (!firstSelectable)
            {
                return true; // event consumed, but nothing todo.
            }

            if (event.shiftKey())
            {
                selectRange(currentSelectionStart_.value_or(*firstSelectable), *firstSelectable);
                return true;
            }
            if (event.ctrlKey())
            {
                select(*firstSelectable);
                return true;
            }
            deselectAll();
            select(*firstSelectable);
            return true;
        }
        if (event.key() == "ArrowLeft" || event.key() == "ArrowUp")
        {
            if (items_->value().empty())
                return true;

            auto currentIndex = currentSelectionStart_;
            if (!currentIndex)
            {
                currentIndex = findLastSelectable();
                if (currentIndex)
                {
                    select(*currentIndex);
                    return true;
                }
            }
            if (!currentIndex)
                return true;

            if ((currentFlavor_ == Flavor::Icons && event.key() == "ArrowLeft") || currentFlavor_ == Flavor::Table)
            {
                if (event.shiftKey() || event.ctrlKey())
                {
                    auto result = findPreviousSelectable(*currentIndex);
                    if (result)
                        select(*result);
                    return true;
                }
                deselectAll();
                auto result = findPreviousSelectable(*currentIndex);
                if (result)
                    select(*result);
                return true;
            }

            // or else its icons and arrow up:
            auto position = GridPosition{*this, *currentIndex};
            position.up();
            if (event.shiftKey())
            {
                // TODO:
                return true;
            }
            if (event.ctrlKey())
            {
                select(position.normalized().toIndex());
                return true;
            }
            deselectAll();
            select(position.normalized().toIndex());
            return true;
        }
        if (event.key() == "ArrowRight" || event.key() == "ArrowDown")
        {
            if (items_->value().empty())
                return true;

            auto currentIndex = currentSelectionStart_;
            if (!currentIndex)
            {
                currentIndex = findFirstSelectable();
                if (currentIndex)
                {
                    select(*currentIndex);
                    return true;
                }
            }
            if (!currentIndex)
                return true;

            if ((currentFlavor_ == Flavor::Icons && event.key() == "ArrowRight") || currentFlavor_ == Flavor::Table)
            {
                if (event.shiftKey() || event.ctrlKey())
                {
                    auto result = findNextSelectable(*currentIndex);
                    if (result)
                        select(*result);
                    return true;
                }
                deselectAll();
                auto result = findNextSelectable(*currentIndex);
                if (result)
                    select(*result);
                return true;
            }

            // or else its icons and arrow down:
            // TODO:
            auto position = GridPosition{*this, *currentIndex};
            position.down();
            if (event.shiftKey())
            {
                // TODO:
                return true;
            }
            if (event.ctrlKey())
            {
                select(position.normalized().toIndex());
                return true;
            }
            deselectAll();
            select(position.normalized().toIndex());
            return true;
        }

        // ....
        return false;
    }
    SelectionManager::GridPosition SelectionManager::calculateGridPositionFromIndex(std::size_t index) const
    {
        return GridPosition{*this, index};
    }
    std::size_t SelectionManager::itemsInLastRow() const
    {
        if (gridColumns_ == 0)
            return 0;
        const auto totalItems = items_->value().size();
        const auto fullRows = totalItems / gridColumns_;
        const auto itemsInFullRows = fullRows * gridColumns_;
        const auto diff = totalItems - itemsInFullRows;
        if (diff == 0)
            return gridColumns_;
        return diff;
    }
    bool SelectionManager::isIndexUnselected(std::make_signed_t<std::size_t> index) const
    {
        if (index < 0)
            return false;
        if (index >= static_cast<std::make_signed_t<std::size_t>>(items_->value().size()))
            return false;
        return !isSelected(static_cast<std::size_t>(index));
    }
    bool SelectionManager::isIndexUnselected(std::size_t index) const
    {
        if (index >= items_->value().size())
            return false;
        return !isSelected(index);
    }
    std::optional<std::size_t> SelectionManager::findNextSelectable(std::size_t index) const
    {
        auto newIndex = index + 1;

        for (; newIndex < items_->value().size() && !isIndexUnselected(newIndex); ++newIndex)
        {}

        if (newIndex < items_->value().size())
            return newIndex;
        else
        {
            // look from beginning:
            auto firstSelectable = findFirstSelectable();
            if (!firstSelectable)
                return std::nullopt;

            newIndex = *firstSelectable;
            for (; newIndex < index && !isIndexUnselected(newIndex); ++newIndex)
            {}

            if (newIndex < index)
                return newIndex;
        }
        return std::nullopt;
    }
    std::optional<std::size_t> SelectionManager::findPreviousSelectable(std::size_t index) const
    {
        auto newIndex = static_cast<std::make_signed_t<std::size_t>>(index) - 1;
        auto signedIndex = static_cast<std::make_signed_t<std::size_t>>(index);

        for (; newIndex >= 0 && !isIndexUnselected(newIndex); --newIndex)
        {}

        if (newIndex >= 0)
            return static_cast<std::size_t>(newIndex);
        else
        {
            // look from end:
            auto lastSelectable = findLastSelectable();
            if (!lastSelectable)
                return std::nullopt;

            newIndex = static_cast<std::make_signed_t<std::size_t>>(*lastSelectable);
            for (; newIndex > signedIndex && !isIndexUnselected(newIndex); --newIndex)
            {}

            if (newIndex > signedIndex)
                return static_cast<std::size_t>(newIndex);
        }
        return std::nullopt;
    }

    void SelectionManager::selectRange(std::size_t begin, std::size_t endInclusive)
    {
        if (items_->value().empty())
            return;
        if (begin == endInclusive)
        {
            select(begin);
            return;
        }
        if (endInclusive >= items_->value().size())
            endInclusive = items_->value().size() - 1;
        if (begin >= items_->value().size())
            begin = items_->value().size() - 1;
        if (begin > endInclusive)
        {
            selectRange(0, endInclusive);
            selectRange(begin, items_->value().size() - 1);
            return;
        }
        for (std::size_t i = begin; i <= endInclusive; ++i)
            select(i);
    }

    std::optional<std::size_t> SelectionManager::findFirstSelectable() const
    {
        if (items_->value().empty())
            return std::nullopt;
        return 0;
    }
    std::optional<std::size_t> SelectionManager::findLastSelectable() const
    {
        if (items_->value().empty())
            return std::nullopt;
        return items_->value().size() - 1;
    }
}