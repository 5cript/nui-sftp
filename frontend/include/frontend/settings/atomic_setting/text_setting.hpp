#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/input.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/value.hpp>
#include <nui/frontend/attributes/disabled.hpp>

template <bool Disengageable = false>
class TextSetting : public Setting<Disengageable, std::string>
{
  public:
    using SettingBase = Setting<Disengageable, std::string>;

    using SettingBase::state_;
    using SettingBase::inheritedState_;
    using SettingBase::inheritanceStatus_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::observeEngagedToBool;

    using SettingBase::SettingBase;

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            input{
                class_ = "setting-input",
                value = SettingBase::observedValueWithInheritance(),
                observeEngagedToBool(disabled),
                "blur"_event = [this](Nui::val event){
                    state_ = event["target"]["value"].as<std::string>();
                    onChange_();
                }
            }(),
            reset(),
            help()
        );
        // clang-format on
    }
};