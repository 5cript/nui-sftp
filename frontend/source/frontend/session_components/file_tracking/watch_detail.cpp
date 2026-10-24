#include <frontend/session_components/file_tracking/watch_detail.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer watchDetail(
        [[maybe_unused]] TrackingStore& store,
        [[maybe_unused]] TrackingActions const& actions,
        [[maybe_unused]] std::shared_ptr<WatchItem> const& item
    )
    {
        return Nui::nil();
    }
}
