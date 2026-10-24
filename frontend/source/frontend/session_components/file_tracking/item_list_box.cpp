#include <frontend/session_components/file_tracking/item_list_box.hpp>

#include <nui/frontend/elements/nil.hpp>

namespace FileTracking
{
    Nui::ElementRenderer
    itemListBox([[maybe_unused]] ItemListBoxOptions options, [[maybe_unused]] Nui::ElementRenderer content)
    {
        return Nui::nil();
    }
}
