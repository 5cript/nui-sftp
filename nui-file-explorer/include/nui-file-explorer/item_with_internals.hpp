#pragma once

#include <nui-file-explorer/item.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <memory>

namespace NuiFileExplorer
{
    struct ItemWithInternals
    {
        Item item;
        std::shared_ptr<Nui::Observed<bool>> selected;
        std::weak_ptr<Nui::Dom::BasicElement> element;

        void select()
        {
            *selected = true;
        }

        explicit ItemWithInternals(Item const& item)
            : item{item}
            , selected{std::make_shared<Nui::Observed<bool>>(false)}
            , element{}
        {}
    };
}