#include <frontend/session_components/file_tracking/outliner.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer
    outliner([[maybe_unused]] TrackingStore& store, [[maybe_unused]] TrackingActions const& actions)
    {
        return Nui::nil();
    }
}
