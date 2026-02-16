#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <frontend/color.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>
#include <ids/id.hpp>

#include <script-nui-components/color_picker.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

template <bool Disengageable = false>
class ColorSetting : public Setting<Disengageable, std::string>
{
  public:
    using SettingBase = Setting<Disengageable, std::string>;

    using SettingBase::value;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::observeEngagedToBool;
    using SettingBase::stateWithInheritance_;

    using SettingBase::SettingBase;

    /// Converts to #RGBA from rgba(r, g, b, a)
    void reformatColor(std::string const& rgbaCss)
    {
        auto colorOpt = parseCssColor(rgbaCss);
        if (colorOpt)
        {
            value(colorOpt->toPoundSignRGBA());
        }
        else
        {
            Log::warn("ColorSetting", "reformatColor", "Failed to parse color: {}", rgbaCss);
        }
    }

    Nui::ElementRenderer operator()(auto&& labelText)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        const auto idString = Ids::generateId().id();

        // clang-format off
        return div{}(
            SettingBase::label(std::forward<decltype(labelText)>(labelText)),
            ScriptNuiComponents::colorPicker(ScriptNuiComponents::ColorPickerOptions<decltype(stateWithInheritance_)>{
                .value = stateWithInheritance_,
                .attributes = {
                    observeEngagedToBool(disabled),
                },
                .onChange = [this](std::string const& newColor)
                {
                    reformatColor(newColor);
                    onChange_();
                },
            }),
            reset(),
            help()
        );
        // clang-format on
    }
};