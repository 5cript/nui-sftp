#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <paths/local_path.hpp>
#include <utility/lifetime_token.hpp>

namespace FileExplorer
{
    /**
     * @brief Opens the local file @p path with its application. A file that can run code is opened only after the
     *        user confirmed it, when the settings ask for that; errors are shown in @p dialog. Replies that arrive
     *        after @p owner is gone are dropped.
     */
    void openLocalFile(
        Utility::LifetimeToken const& owner,
        ConfirmDialog& dialog,
        Paths::LocalPath const& path,
        bool openWith,
        std::function<void()> onDeclined = {}
    );
}
