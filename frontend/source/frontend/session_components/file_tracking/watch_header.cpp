#include <frontend/session_components/file_tracking/watch_header.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer watchHeader(
        [[maybe_unused]] TrackingStore& store,
        [[maybe_unused]] TrackingActions const& actions,
        [[maybe_unused]] std::shared_ptr<WatchItem> const& item
    )
    {
        return Nui::nil();
    }
}
