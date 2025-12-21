#include <nui-file-explorer/flavor.hpp>

namespace NuiFileExplorer
{
    std::string fileGridFlavorToString(Flavor value)
    {
        using enum Flavor;
        switch (value)
        {
            case Icons:
                return "icons";
            case Table:
                return "table";
            case Tiles:
                return "tiles";
        }
    }
    Flavor fileGridFlavorFromString(std::string const& value)
    {
        using enum Flavor;
        if (value == "icons")
            return Icons;
        if (value == "table")
            return Table;
        if (value == "tiles")
            return Tiles;
        return Icons;
    }
}