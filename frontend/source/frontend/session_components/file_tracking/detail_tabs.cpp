#include <frontend/session_components/file_tracking/detail_tabs.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    std::vector<DetailTabEntry> detailTabRegistry()
    {
        return {};
    }

    Nui::ElementRenderer detailTabs(
        [[maybe_unused]] TrackingStore& store,
        [[maybe_unused]] TrackingActions const& actions,
        [[maybe_unused]] std::shared_ptr<WatchItem> const& item
    )
    {
        return Nui::nil();
    }
}
