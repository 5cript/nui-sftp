#pragma once

#include <string>

namespace NuiFileExplorer
{
    enum class Flavor
    {
        Icons, // Customizable Size, size below certain size will flex wrap differently
        Table
    };

    std::string fileGridFlavorToString(Flavor value);
    Flavor fileGridFlavorFromString(std::string const& value);
}