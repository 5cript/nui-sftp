#pragma once

#include <nui-file-explorer/item.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <memory>

namespace NuiFileExplorer
{
    class SelectionManager;

    class ItemWithInternals
    {
      public:
        friend SelectionManager;

        bool isSelected() const
        {
            return selected->value();
        }

        explicit ItemWithInternals(Item const& item)
            : item{item}
            , element{}
            , selected{std::make_shared<Nui::Observed<bool>>(false)}
        {}

        std::optional<int> indexDataAttribute() const
        {
            auto elementLocked = element.lock();
            if (!elementLocked)
                return std::nullopt;

            auto val = elementLocked->val()["dataset"]["index"];
            if (val.isUndefined() || val.isNull())
                return std::nullopt;

            return std::stol(val.as<std::string>());
        }

        bool isSelectable() const
        {
            return item.path.filename() != "..";
        }

        auto observeSelected(auto&& fn) const
        {
            return Nui::observe(selected).generate(std::forward<decltype(fn)>(fn));
        }

      public:
        Item item;
        std::weak_ptr<Nui::Dom::BasicElement> element;

      private:
        // Private so no one can bypass the selection manager:
        std::shared_ptr<Nui::Observed<bool>> selected;
    };
}