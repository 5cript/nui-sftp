#pragma once

#include <nui-file-explorer/item.hpp>

#include <optional>

enum class SyncItemAction
{
    Upload,
    Download,
    DeleteLocal,
    DeleteRemote
};

struct SyncItem
{
    SyncItemAction action;
    std::optional<NuiFileExplorer::Item> localItem;
    std::optional<NuiFileExplorer::Item> remoteItem;
};
