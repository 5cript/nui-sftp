#pragma once

#include <frontend/settings/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/input.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

template <typename ValueType, bool Disengageable = false>
class NumberSetting : public Setting<Disengageable, ValueType>
{
  public:
    using SettingBase = Setting<Disengageable, ValueType>;

    using SettingBase::state_;
    using SettingBase::engaged_;
    using SettingBase::inheritedState_;
    using SettingBase::inheritanceStatus_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::observeEngagedToBool;

    using SettingBase::SettingBase;

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            ui5::input{
                "type"_prop = "Number",
                // FIXME: I convert to a string here because of an error message
                // ListItemStandardExpandableTextTemplate.1a93b8ba.js:3172  [UI5-FWK] numeric value for property [value] of component [ui5-input]
                // is missing "{ type: Number }" in its property decorator. Attribute conversion will treat it as a string.
                // If this is intended, pass the value converted to string, otherwise add the type to the property decorator
                "value"_prop = Nui::observe(engaged_, state_, inheritedState_, inheritanceStatus_).generate(
                    [](bool engaged, ValueType const& value, std::optional<ValueType> const& inheritedValue, SettingBase::InheritanceStatus status) {
                        if (!engaged && inheritedValue && status == SettingBase::InheritanceStatus::AncestorEngaged)
                            return std::to_string(*inheritedValue);
                        return std::to_string(value);
                    }
                ),
                observeEngagedToBool("disabled"_prop),
                "change"_event = [this](Nui::val event){
                    state_ = static_cast<ValueType>(event["target"]["value"].as<int>());
                    onChange_();
                },
            }(),
            reset(),
            help()
        );
        // clang-format on
    }
};