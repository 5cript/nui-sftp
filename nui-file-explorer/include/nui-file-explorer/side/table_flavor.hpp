#pragma once

#include <nui-file-explorer/side/flavor_implementation.hpp>

namespace NuiFileExplorer
{
    class TableFlavor : public FlavorImplementation
    {
      public:
        TableFlavor(Side& impl);
        Nui::ElementRenderer operator()() override;
    };
}