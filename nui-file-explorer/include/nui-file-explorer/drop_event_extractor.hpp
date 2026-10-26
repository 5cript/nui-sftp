#pragma once

#include <nui/frontend/api/drag_event.hpp>
#include <nui-file-explorer/item.hpp>
#include <nui-file-explorer/side_model_interface.hpp>

namespace NuiFileExplorer
{
    struct DropEventResult
    {
        /// This drop was handed to the backend, which returns it via rpc elsewhere.
        bool delegatedToBackend = false;
        std::optional<std::vector<Item>> externDroppedItems = std::nullopt;
        std::optional<bool> isInternalDropFromLeftSide = std::nullopt;
        /** @brief Where the items go below the directory of the side they were dropped on, see transferTargetOf. */
        std::optional<std::string> internalDropSubdir = std::nullopt;
        bool issueWebkitWarning = false;
    };

    /**
     * @brief What the modifier keys held during @p event ask for. Items of a side move unless Ctrl, or Alt (Option on
     *        macOS), asks for a copy; items from the desktop are copied unless Shift, or Cmd on macOS, asks for a move.
     */
    DropAction suggestedDropAction(Nui::WebApi::DragEvent const& event, bool fromDesktop);

    std::optional<DropEventResult>
    extractDropEvent(Nui::WebApi::DragEvent event, ISideModel& sideModel, std::optional<Item> const& droppedOnItem);
}