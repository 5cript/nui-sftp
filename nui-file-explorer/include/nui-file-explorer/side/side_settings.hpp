#pragma once

#include <functional>

namespace NuiFileExplorer
{
    struct SideSettings
    {
        bool pathBarOnTop = false;
        bool showHiddenFiles = false;
        std::function<void(bool)> onShowHiddenFilesChanged;

        // Items per page in the pagination footer. The footer hides itself when the
        // directory (or filtered match set) fits within one page.
        int pageSize = 500;
    };
}
