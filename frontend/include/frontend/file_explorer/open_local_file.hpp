#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <paths/local_path.hpp>

namespace FileExplorer
{
    /**
     * @brief Opens the local file @p path with its application. A file that can run code is opened only after the
     *        user confirmed it, when the settings ask for that; errors are shown in @p dialog.
     */
    void openLocalFile(ConfirmDialog& dialog, Paths::LocalPath const& path, bool openWith);
}
