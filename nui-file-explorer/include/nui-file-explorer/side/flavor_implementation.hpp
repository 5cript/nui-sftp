#pragma once

#include <nui-file-explorer/side/side_implementation.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/api/drag_event.hpp>

namespace NuiFileExplorer
{
    class Side;

    class FlavorImplementation
    {
      public:
        FlavorImplementation(Side& side, Side* otherSide);
        virtual ~FlavorImplementation() = default;

        virtual Nui::ElementRenderer operator()() = 0;

        void onDrop(Nui::WebApi::DragEvent event, std::optional<Item> const& droppedOnItem);

      protected:
        SideImplementation& impl() const;
        SideImplementation* otherImpl() const;

        /**
         *  @brief Resolve the item index from an event target by walking up to the nearest `[data-index]` ancestor.
         *  @param target The event target (typically `event.val()["target"]`).
         *  @return Item index, or -1 if no item ancestor was found.
         */
        int resolveItemIndex(Nui::val target);

        /**
         *  @brief Update which item currently shows the drop-hover highlight, clearing the previous one.
         *  @param index Item index to highlight, or -1 to clear. Negative / out-of-range / non-directory items clear.
         */
        void setHoverItem(int index);

        void onDelegatedDragStart(Nui::WebApi::DragEvent event);
        void onDelegatedDragOver(Nui::WebApi::DragEvent event);
        void onDelegatedDragLeave(Nui::WebApi::DragEvent event);
        void onDelegatedDrop(Nui::WebApi::DragEvent event);

        Side* side_;
        Side* otherSide_;
        int currentHoverIndex_{-1};
    };
}