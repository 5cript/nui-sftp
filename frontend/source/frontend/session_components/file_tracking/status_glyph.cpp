#include <frontend/session_components/file_tracking/status_glyph.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer statusGlyph([[maybe_unused]] WatchStatus status)
    {
        return Nui::nil();
    }

    Nui::ElementRenderer orphanGlyph()
    {
        return Nui::nil();
    }
}
