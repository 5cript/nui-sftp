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
        enum class SearchHighlight
        {
            Off,
            Highlight,
            Muted
        };

        friend SelectionManager;

        bool isSelected() const
        {
            return selected_->value();
        }

        explicit ItemWithInternals(Item const& item)
            : item{item}
            , element{}
            , searchHighlighted_{std::make_shared<Nui::Observed<SearchHighlight>>(SearchHighlight::Off)}
            , isDropHovered_{std::make_shared<Nui::Observed<bool>>(false)}
            , selected_{std::make_shared<Nui::Observed<bool>>(false)}
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

        auto observeClassRelevant(auto&& fn) const
        {
            return Nui::observe(selected_, searchHighlighted_, isDropHovered_).generate(std::forward<decltype(fn)>(fn));
        }

        auto observeSelected(auto&& fn) const
        {
            return Nui::observe(selected_).generate(std::forward<decltype(fn)>(fn));
        }

        SearchHighlight searchHighlight() const
        {
            return searchHighlighted_->value();
        }

        void searchHighlight(SearchHighlight value)
        {
            *searchHighlighted_ = value;
        }

        bool isDropHovered() const
        {
            return isDropHovered_->value();
        }

        void isDropHovered(bool value)
        {
            *isDropHovered_ = value;
        }

      public:
        Item item;
        std::weak_ptr<Nui::Dom::BasicElement> element;

      private:
        std::shared_ptr<Nui::Observed<SearchHighlight>> searchHighlighted_;
        std::shared_ptr<Nui::Observed<bool>> isDropHovered_;
        std::shared_ptr<Nui::Observed<bool>> selected_;
    };
}