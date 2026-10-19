#pragma once

#include <nui-file-explorer/texts.hpp>

#include <functional>
#include <optional>

namespace NuiFileExplorer
{
    /**
     * @brief Which machine a side shows, labelled in the path bar and on top of Places.
     */
    enum class SideOrigin
    {
        Local,
        Remote
    };

    struct SideSettings
    {
        bool pathBarOnTop = false;
        bool showHiddenFiles = false;
        std::function<void(bool)> onShowHiddenFilesChanged;

        // Items per page in the pagination footer. The footer hides itself when the
        // directory (or filtered match set) fits within one page.
        int pageSize = 500;

        // Texts the side renders, English unless translations are passed.
        Texts texts{};

        /**
         * @brief Shows the origin chip and card when set. The user and host are set through
         * Side::originHost.
         */
        std::optional<SideOrigin> origin{std::nullopt};
    };
}
