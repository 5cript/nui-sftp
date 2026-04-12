#pragma once

#include <nui-file-explorer/item.hpp>
#include <nui/event_system/observed_value.hpp>

#include <memory>
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
    // null = not yet enqueued; 0.0–1.0 = in progress; > 1.0 = completed
    std::shared_ptr<Nui::Observed<double>> progress;
};
