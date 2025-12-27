#pragma once

#include <nui-file-explorer/side/side_implementation.hpp>

#include <nui/frontend/element_renderer.hpp>

namespace NuiFileExplorer
{
    class Side;

    class FlavorImplementation
    {
      public:
        FlavorImplementation(Side& side);
        virtual ~FlavorImplementation() = default;

        virtual Nui::ElementRenderer operator()() = 0;

      protected:
        SideImplementation& impl() const;
        Side* side_;
    };
}