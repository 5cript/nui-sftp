#include <nui-file-explorer/side/flavor_implementation.hpp>
#include <nui-file-explorer/side.hpp>

namespace NuiFileExplorer
{
    FlavorImplementation::FlavorImplementation(Side& side)
        : side_{&side}
    {}
    SideImplementation& FlavorImplementation::impl() const
    {
        return *side_->impl_;
    }
}