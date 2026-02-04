#pragma once

#include <nui/frontend/api/drag_event.hpp>
#include <nui-file-explorer/item.hpp>
#include <nui-file-explorer/side_model_interface.hpp>

namespace NuiFileExplorer
{
    struct DropEventResult
    {
        /// This drop was handled and will return via rpc elsewhere.
        bool delegatedToWebView2 = false;
        std::optional<std::vector<Item>> externDroppedItems = std::nullopt;
        std::optional<bool> isInternalDropFromLeftSide = std::nullopt;
        std::optional<std::string> internalDropSubdir = std::nullopt;
        bool issueWebkitWarning = false;
    };

    std::optional<DropEventResult>
    extractDropEvent(Nui::WebApi::DragEvent event, ISideModel& sideModel, std::optional<Item> const& droppedOnItem);
}