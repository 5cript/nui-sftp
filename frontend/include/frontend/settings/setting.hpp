#pragma once

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <traits/functions.hpp>
#include <ui5/components/button.hpp>
#include <ui5/components/responsive_popover.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/reference.hpp>
#include <nui/frontend/elements/div.hpp>

#include <concepts>

template <typename ValueType>
class Setting
{
  public:
    Setting(
        Nui::Observed<ValueType>& state,
        std::string helpText,
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

        return div{}(
            ui5::button{
                "design"_prop = "Transparent",
                "icon"_prop = "sys-help",
                "click"_event =
                    [this]()
                {
                    isHelpOpen_ = !*isHelpOpen_;
                },
            }(),
            ui5::responsive_popover{
                "opener"_prop = "btn",
                "header-text"_prop = "Help",
                "open"_prop = isHelpOpen_,
            }(helpText_)
        );
    }

  protected:
    Nui::Observed<ValueType>* state_;
    std::function<void()> onChange_;
    std::function<void()> resetAction_;
    std::string helpText_;

  private:
    Nui::Observed<bool> isHelpOpen_{false};
};