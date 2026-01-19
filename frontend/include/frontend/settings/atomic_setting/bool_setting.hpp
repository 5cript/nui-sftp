#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/switch.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

template <bool Disengageable = false>
class BoolSetting : public Setting<Disengageable, bool>
{
  public:
    using SettingBase = Setting<Disengageable, bool>;

    using SettingBase::state_;
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
            ui5::switch_{
                "checked"_prop = SettingBase::observedValueWithInheritance(),
                observeEngagedToBool("disabled"_prop),
                "change"_event = [this](Nui::val event){
                    state_ = event["target"]["checked"].as<bool>();
                    onChange_();
                }
            }(),
            reset(),
            help()
        );
        // clang-format on
    }
};
