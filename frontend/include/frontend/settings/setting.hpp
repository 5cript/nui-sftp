#pragma once

#include <utility/language.hpp>
#include <ids/id.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <traits/functions.hpp>
#include <ui5/components/button.hpp>
#include <ui5/components/responsive_popover.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/reference.hpp>
#include <nui/frontend/attributes/id.hpp>
#include <nui/frontend/elements/div.hpp>

#include <concepts>

template <typename ValueType>
class Setting
{
  public:
    Setting(
        Nui::Observed<ValueType>& state,
        LanguageObservedText helpText,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction
    )
        : state_{&state}
        , onChange_{std::forward<decltype(onChange)>(onChange)}
        , resetAction_{std::forward<decltype(resetAction)>(resetAction)}
        , helpText_{std::move(helpText)}
    {}
    virtual ~Setting() = default;

    Nui::ElementRenderer reset()
    {
        using namespace Nui::Attributes;

        return ui5::button{
            "design"_prop = "Transparent",
            "icon"_prop = "refresh",
            "tooltip"_prop = "Reset to default value",
            "click"_event = [this]()
            {
                resetAction_();
            },
        }();
    }

    Nui::ElementRenderer help()
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        const auto idString = Ids::generateId().id();

        return div{}(
            ui5::button{
                "design"_prop = "Transparent",
                "icon"_prop = "sys-help",
                id = idString,
                "click"_event =
                    [this]()
                {
                    if (auto helpPopover = helpPopoverElement_.lock(); helpPopover)
                    {
                        helpPopover->val().set("open", !helpPopover->val()["open"].as<bool>());
                    }
                },
            }(),
            ui5::responsive_popover{
                reference =
                    [this](std::weak_ptr<Nui::Dom::BasicElement> const& ptr)
                {
                    helpPopoverElement_ = ptr;
                },
                "opener"_prop = idString,
                "header-text"_prop = "Help"
            }(helpText_)
        );
    }

  protected:
    Nui::Observed<ValueType>* state_;
    std::function<void()> onChange_;
    std::function<void()> resetAction_;
    LanguageObservedText helpText_;

  private:
    std::weak_ptr<Nui::Dom::BasicElement> helpPopoverElement_;
};