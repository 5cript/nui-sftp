#include <frontend/session_components/file_tracking/sync_progress_block.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer syncProgressBlock(
        [[maybe_unused]] TrackingStore& store,
        [[maybe_unused]] TrackingActions const& actions,
        [[maybe_unused]] std::shared_ptr<WatchItem> const& item
    )
    {
        return Nui::nil();
    }
}
