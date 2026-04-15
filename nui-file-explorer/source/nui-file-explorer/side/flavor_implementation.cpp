#include "nui/frontend/api/console.hpp"
#include <nui-file-explorer/side/flavor_implementation.hpp>
#include <nui-file-explorer/side.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <nui-file-explorer/drop_event_extractor.hpp>

#include <nui/frontend/api/json.hpp>

#include <nlohmann/json.hpp>

namespace NuiFileExplorer
{
    FlavorImplementation::FlavorImplementation(Side& side, Side* otherSide)
        : side_{&side}
        , otherSide_{otherSide}
    {}
    SideImplementation& FlavorImplementation::impl() const
    {
        return *side_->impl_;
    }
    SideImplementation* FlavorImplementation::otherImpl() const
    {
        return otherSide_ ? &*otherSide_->impl_ : nullptr;
    }
    void FlavorImplementation::onDrop(Nui::WebApi::DragEvent event, std::optional<Item> const& droppedOnItem)
    {
        event.stopPropagation();
        event.preventDefault();

        auto eventExtracted = extractDropEvent(event, side_->model(), droppedOnItem);
        if (!eventExtracted.has_value())
            return;

        if (eventExtracted->delegatedToWebView2)
            return;

        if (eventExtracted->externDroppedItems.has_value())
        {
            if (side_->model().isLeft())
            {
                side_->model().onError(
                    "Dropping external items on the local side is not supported yet. Copy files via the system means."
                );
                return;
            }
            Nui::WebApi::Console::log("External items dropped: ", eventExtracted->externDroppedItems->size());
            if (otherImpl())
            {
                otherSide_->model().onDropExternal(
                    *eventExtracted->externDroppedItems,
                    eventExtracted->internalDropSubdir,
                    eventExtracted->issueWebkitWarning
                );
            }
            return;
        }

        if (eventExtracted->isInternalDropFromLeftSide.has_value())
        {
            if (side_->model().isLeft() != *eventExtracted->isInternalDropFromLeftSide)
            {
                // its a transfer!
                if (otherImpl())
                {
                    otherSide_->model().onTransfer(
                        otherImpl()->selectionManager.selectedItems(), eventExtracted->internalDropSubdir
                    );
                }
            }
            else
            {
                // TODO: else its a move within the same side.
                side_->model().onError("drag and drop on the same side is not implemented");
            }
        }
    }

    int FlavorImplementation::resolveItemIndex(Nui::val target)
    {
        if (target.isNull() || target.isUndefined())
            return -1;
        auto node = target.call<Nui::val>("closest", "[data-index]"s);
        if (node.isNull() || node.isUndefined())
            return -1;
        auto idxAttr = node.call<Nui::val>("getAttribute", "data-index"s);
        if (idxAttr.isNull() || idxAttr.isUndefined())
            return -1;
        try
        {
            return std::stoi(idxAttr.as<std::string>());
        }
        catch (...)
        {
            return -1;
        }
    }

    void FlavorImplementation::setHoverItem(int index)
    {
        auto& items = impl().items.value();
        int next = index;
        if (next >= 0 &&
            (static_cast<std::size_t>(next) >= items.size() || !items[next].item.isDirectoryLike()))
        {
            next = -1;
        }
        if (next == currentHoverIndex_)
            return;
        if (currentHoverIndex_ >= 0 && static_cast<std::size_t>(currentHoverIndex_) < items.size())
            items[currentHoverIndex_].isDropHovered(false);
        if (next >= 0)
            items[next].isDropHovered(true);
        currentHoverIndex_ = next;
    }

    void FlavorImplementation::onDelegatedDragStart(Nui::WebApi::DragEvent event)
    {
        event.stopPropagation();

        // Preserve prior UX: only items that are part of the current selection can initiate a drag.
        const int index = resolveItemIndex(event.val()["target"]);
        auto& items = impl().items.value();
        if (index < 0 || static_cast<std::size_t>(index) >= items.size() || !items[index].isSelected())
        {
            event.preventDefault();
            return;
        }

        auto dataTransferOpt = event.dataTransfer();
        if (!dataTransferOpt.has_value())
            return;

        Nui::val info = Nui::val::object();
        info.set("isLeft", side_->model().isLeft());
        dataTransferOpt->setData("application/json", Nui::JSON::stringify(info));
    }

    void FlavorImplementation::onDelegatedDragOver(Nui::WebApi::DragEvent event)
    {
        event.val().call<void>("preventDefault");
        setHoverItem(resolveItemIndex(event.val()["target"]));
    }

    void FlavorImplementation::onDelegatedDragLeave(Nui::WebApi::DragEvent event)
    {
        auto related = event.val()["relatedTarget"];
        auto current = event.val()["currentTarget"];
        if (!related.isNull() && !related.isUndefined() && !current.isNull() && !current.isUndefined() &&
            current.call<bool>("contains", related))
        {
            return;
        }
        setHoverItem(-1);
    }

    void FlavorImplementation::onDelegatedDrop(Nui::WebApi::DragEvent event)
    {
        const int index = resolveItemIndex(event.val()["target"]);
        setHoverItem(-1);

        auto& items = impl().items.value();
        std::optional<Item> droppedOn;
        if (index >= 0 && static_cast<std::size_t>(index) < items.size())
            droppedOn = items[index].item;
        onDrop(std::move(event), droppedOn);
    }
}