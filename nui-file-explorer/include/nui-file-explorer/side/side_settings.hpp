#pragma once

#include <functional>

namespace NuiFileExplorer
{
    struct SideSettings
    {
        bool pathBarOnTop = false;
        bool showHiddenFiles = false;
        std::function<void(bool)> onShowHiddenFilesChanged;
    };
}
