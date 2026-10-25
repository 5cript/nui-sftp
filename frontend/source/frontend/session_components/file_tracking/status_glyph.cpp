#include <frontend/session_components/file_tracking/status_glyph.hpp>
#include <utility/enum_string_convert.hpp>

#include <ui5-sap-icons/icons/accept.hpp>
#include <ui5-sap-icons/icons/document.hpp>
#include <ui5-sap-icons/icons/status-inactive.hpp>
#include <ui5-sap-icons/icons/sys-help.hpp>
#include <ui5-sap-icons/icons/warning.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <string>
#include <utility>

namespace FileTracking
{
    namespace
    {
        Nui::ElementRenderer iconOf(WatchStatus status)
        {
            switch (status)
            {
                case WatchStatus::Conflicts:
                    return Ui5Icons::sys_help();
                case WatchStatus::Failed:
                    return Ui5Icons::warning();
                case WatchStatus::Stopped:
                    return Ui5Icons::status_inactive();
                case WatchStatus::Watching:
                case WatchStatus::Syncing:
                    break;
            }
            return Ui5Icons::accept();
        }

        Nui::ElementRenderer glyph(std::string status, Nui::ElementRenderer icon)
        {
            using namespace Nui::Attributes;
            using Nui::Elements::span;

            // clang-format off
            return span{
                class_ = "file-tracking-glyph",
                "data-status"_attr = std::move(status),
                "aria-hidden"_attr = "true",
            }(
                std::move(icon)
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer statusGlyph(WatchStatus status)
    {
        return glyph(Utility::enumToString(status), iconOf(status));
    }

    Nui::ElementRenderer orphanGlyph()
    {
        return glyph("Orphaned", Ui5Icons::document());
    }
}
