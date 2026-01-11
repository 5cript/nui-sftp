#pragma once

#include <frontend/settings/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/input.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

class TextSetting : public Setting<std::string>
{
  public:
    using Setting<std::string>::state_;
    using Setting<std::string>::onChange_;

    TextSetting(
        Nui::Observed<std::string>& state,
        LanguageObservedText helpText,
        std::invocable auto&& onChange,
        std::invocable auto&& resetAction
    )
        : Setting<std::string>{
              state,
              std::move(helpText),
              std::forward<decltype(onChange)>(onChange),
              std::forward<decltype(resetAction)>(resetAction)
          }
    {}

    Nui::ElementRenderer operator()(auto&& label)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            ui5::label{
                style = "color: var(--sapTextColor); margin-right: 10px",
            }(std::forward<decltype(label)>(label)),
            ui5::input{
                "value"_prop = *state_,
                "change"_event = [this](Nui::val event){
                    *state_ = event["target"]["value"].as<std::string>();
                    onChange_();
                }
            }(),
            reset(),
            help()
        );
        // clang-format on
    }
};