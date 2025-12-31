#include <nui-file-explorer/side/flavor_implementation.hpp>
#include <nui-file-explorer/side.hpp>

#include <nui/frontend/api/json.hpp>

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

        auto dataTransferOpt = event.dataTransfer();
        if (!dataTransferOpt.has_value())
        {
            Nui::WebApi::Console::log("No data transfer in drop event.");
            return;
        }

        std::optional<std::string> subdir = std::nullopt;

        if (droppedOnItem && droppedOnItem->type == Item::Type::Directory)
            subdir = droppedOnItem->path.filename().string();

        auto info = Nui::JSON::parse(dataTransferOpt->getData("application/json"s));
        if (side_->model().isLeft() != info["isLeft"].as<bool>())
        {
            // its a transfer!
            otherSide_->model().onTransfer(otherImpl().selectionManager.selectedItems(), subdir);
        }
        else
        {
            // TODO: else its a move within the same side.
            side_->model().onError("drag and drop on the same side is not implemented");
        }
    }
}