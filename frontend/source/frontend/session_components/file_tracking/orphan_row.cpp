#include <frontend/session_components/file_tracking/orphan_row.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer orphanRow(
        [[maybe_unused]] TrackingStore& store,
        [[maybe_unused]] TrackingActions const& actions,
        [[maybe_unused]] std::shared_ptr<OrphanItem> const& item
    )
    {
        return Nui::nil();
    }
}
