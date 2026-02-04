#include "nui/frontend/api/console.hpp"
#include <nui-file-explorer/side/flavor_implementation.hpp>
#include <nui-file-explorer/side.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <nui-file-explorer/drop_event_extractor.hpp>

#include <nui/frontend/api/json.hpp>

#include <nlohmann/json.hpp>

namespace NuiFileExplorer
{
    FlavorImplementation::FlavorImplementation(Side& side, Side& otherSide)
        : side_{&side}
        , otherSide_{&otherSide}
    {}
    SideImplementation& FlavorImplementation::impl() const
    {
        return *side_->impl_;
    }
    SideImplementation& FlavorImplementation::otherImpl() const
    {
        return *otherSide_->impl_;
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
            otherSide_->model().onDropExternal(
                *eventExtracted->externDroppedItems,
                eventExtracted->internalDropSubdir,
                eventExtracted->issueWebkitWarning
            );
            return;
        }

        if (eventExtracted->isInternalDropFromLeftSide.has_value())
        {
            if (side_->model().isLeft() != *eventExtracted->isInternalDropFromLeftSide)
            {
                // its a transfer!
                otherSide_->model().onTransfer(
                    otherImpl().selectionManager.selectedItems(), eventExtracted->internalDropSubdir
                );
            }
            else
            {
                // TODO: else its a move within the same side.
                side_->model().onError("drag and drop on the same side is not implemented");
            }
        }
    }
}