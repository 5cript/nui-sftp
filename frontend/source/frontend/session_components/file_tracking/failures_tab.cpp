#include <frontend/session_components/file_tracking/failures_tab.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer failuresTab(
        [[maybe_unused]] TrackingStore& store,
        [[maybe_unused]] TrackingActions const& actions,
        [[maybe_unused]] std::shared_ptr<WatchItem> const& item
    )
    {
        return Nui::nil();
    }
}
