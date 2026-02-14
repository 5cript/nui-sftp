#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/switch.hpp>

#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/input.hpp>
#include <nui/frontend/attributes/style.hpp>
#include <nui/frontend/attributes/type.hpp>
#include <nui/frontend/attributes/checked.hpp>
#include <nui/frontend/attributes/disabled.hpp>

template <bool Disengageable = false>
class BoolSetting : public Setting<Disengageable, bool>
{
  public:
    using SettingBase = Setting<Disengageable, bool>;

    using SettingBase::stateWithInheritance_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::observeEngagedToBool;

    using SettingBase::SettingBase;

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using namespace Nui::Elements;
        using Nui::Elements::div;

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            ScriptNuiComponents::Switch{}(
                ScriptNuiComponents::Switch::Options<decltype(stateWithInheritance_)>{
                    .isChecked = stateWithInheritance_,
                    .attributes = {
                        observeEngagedToBool(disabled)
                    },
                    .onChange = [this](bool, Nui::WebApi::MouseEvent const&){
                        onChange_();
                    },
                }
            ),
            reset(),
            help()
        );
        // clang-format on
    }
};
