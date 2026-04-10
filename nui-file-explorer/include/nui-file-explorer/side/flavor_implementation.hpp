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
        Side* side_;
        Side* otherSide_;
        Nui::Attribute allowDrop = Nui::Attributes::EventFactory{"dragover"} = [](Nui::WebApi::DragEvent event)
        {
            event.val().call<void>("preventDefault");
        };
    };
}