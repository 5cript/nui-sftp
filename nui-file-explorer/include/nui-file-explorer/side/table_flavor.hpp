#pragma once

#include <nui-file-explorer/side/flavor_implementation.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/api/keyboard_event.hpp>

namespace NuiFileExplorer
{
    class TableFlavor : public FlavorImplementation
    {
      public:
        TableFlavor(Side& impl);
        Nui::ElementRenderer operator()() override;
    };
}