#include <frontend/session_components/file_tracking/watch_detail.hpp>
#include <frontend/session_components/file_tracking/detail_tabs.hpp>
#include <frontend/session_components/file_tracking/stop_reason_strip.hpp>
#include <frontend/session_components/file_tracking/sync_progress_block.hpp>
#include <frontend/session_components/file_tracking/watch_header.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

namespace FileTracking
{
    Nui::ElementRenderer
    watchDetail(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        if (!item)
            return Nui::nil();

        // clang-format off
        return div{
            class_ = "file-tracking-detail",
            "data-ft-detail"_attr = "watch",
            "data-ft-watch-id"_attr = item->watchId.value(),
        }(
            watchHeader(store, actions, item),
            stopReasonStrip(store, actions, item),
            syncProgressBlock(store, actions, item),
            div{class_ = "file-tracking-detail-tabs-area"}(
                detailTabs(store, actions, item)
            )
        );
        // clang-format on
    }
}
