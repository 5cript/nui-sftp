#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <utility/language.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/elements/span.hpp>
#include <nui/frontend/attributes/class.hpp>

#include <string>

/**
 * @brief Shows a text the application stored, which the user can only reset, not edit.
 */
class ReadOnlyTextSetting : public Setting<false, std::string>
{
  public:
    using SettingBase = Setting<false, std::string>;

    using SettingBase::SettingBase;

    Nui::ElementRenderer renderRow(LanguageObservedText labelText) override
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return div{}(
            SettingBase::label(std::move(labelText)),
            span{
                class_ = "setting-read-only-text"
            }(
                Nui::observe(stateWithInheritance_).generate([](std::string const& text) {
                    if (text.empty())
                        return language->get("settings", "setting", "noValue");
                    return text;
                })
            ),
            reset(),
            help()
        );
        // clang-format on
    }
};
