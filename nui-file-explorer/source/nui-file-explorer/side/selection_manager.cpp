#include <nui-file-explorer/side/selection_manager.hpp>

#include <nui/event_system/listen.hpp>

namespace NuiFileExplorer
{
    SelectionManager::SelectionManager(Nui::Observed<std::vector<ItemWithInternals>>& items)
        : items_{&items}
    {}

    void SelectionManager::loseTrackToAllSelections()
    {
        selectedIndices_.clear();
        gridWidth_ = 1;
        gridHeight_ = 1;
        currentSelectionStart_ = std::nullopt;
    }
    void SelectionManager::deselectAll()
    {
        for (auto index : selectedIndices_)
        {
            if (items_->value()[index].selected->value())
                *items_->value()[index].selected = false;
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
    void SelectionManager::select(std::size_t index)
    {
        if (index >= items_->value().size())
            return;
        if (!items_->value()[index].isSelectable())
            return;
        if (selectedIndices_.empty())
            currentSelectionStart_ = index;
        selectedIndices_.insert(index);
        *items_->value()[index].selected = true;
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
            return;
        select(static_cast<std::size_t>(*index));
    }
    void SelectionManager::deselect(std::size_t index)
    {
        if (index >= items_->value().size())
            return;
        selectedIndices_.erase(index);
        if (items_->value()[index].selected->value())
            *items_->value()[index].selected = false;
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
        gridWidth_ = width;
        gridHeight_ = height;
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
}