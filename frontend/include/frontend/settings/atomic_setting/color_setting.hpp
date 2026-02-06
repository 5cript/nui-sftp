#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>
#include <frontend/color.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>
#include <ids/id.hpp>

#include <ui5/components/button.hpp>
#include <ui5/components/label.hpp>
#include <ui5/components/input.hpp>
#include <ui5/components/color_palette_popover.hpp>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/impl/attribute_factory.hpp>
#include <nui/frontend/attributes/style.hpp>

template <bool Disengageable = false>
class ColorSetting : public Setting<Disengageable, std::string>
{
  public:
    using SettingBase = Setting<Disengageable, std::string>;

    using SettingBase::state_;
    using SettingBase::inheritedState_;
    using SettingBase::engaged_;
    using SettingBase::inheritanceStatus_;
    using SettingBase::onChange_;
    using SettingBase::reset;
    using SettingBase::help;
    using SettingBase::observeEngagedToBool;
    using SettingBase::observedValueWithInheritance;

    using SettingBase::SettingBase;

    /// Converts to #RGBA from rgba(r, g, b, a)
    void reformatColor(std::string const& rgbaCss)
    {
        auto colorOpt = parseCssColor(rgbaCss);
        if (colorOpt)
        {
            state_ = colorOpt->toPoundSignRGBA();
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
            div{
                class_ = "setting-colorpicker"
            }(
                div{
                    class_ = "setting-colorbox",
                    style = observe(engaged_, state_, inheritedState_, inheritanceStatus_).generate(
                        [](bool engaged, std::string const& colorValue, std::optional<std::string> const& inheritedValue, SettingBase::InheritanceStatus status) {
                            if (!engaged && inheritedValue && status == SettingBase::InheritanceStatus::AncestorEngaged)
                                return "background-color: " + *inheritedValue + ";";
                            return "background-color: " + colorValue + ";";
                        }
                    ),
                }(),
                ui5::input{
                    class_ = "setting-input",
                    "value"_prop = observedValueWithInheritance(),
                    observeEngagedToBool("disabled"_prop),
                    "change"_event = [this](Nui::val event){
                        state_ = static_cast<std::string>(event["target"]["value"].as<std::string>());
                        onChange_();
                    },
                }(),
                ui5::button{
                    "design"_prop = "Transparent",
                    "icon"_prop = "palette",
                    id = idString,
                    observeEngagedToBool("disabled"_prop),
                    "click"_event = [this](){
                        if (auto popover = colorPopover.lock(); popover)
                        {
                            Nui::WebApi::Console::log("Color palette open: ", !popover->val()["open"].as<bool>());
                            popover->val().set("open", !popover->val()["open"].as<bool>());
                        }
                        Nui::WebApi::Console::log("Color palette button clicked");
                    }
                }(),
                ui5::color_palette_popover{
                    observeEngagedToBool("disabled"_prop),
                    reference = [this](std::weak_ptr<Nui::Dom::BasicElement> element) {
                        colorPopover = std::move(element);
                    },
                    "showMoreColors"_prop = true,
                    "showRecentColors"_prop = true,
                    "opener"_prop = idString,
                    "item-click"_event = [this](Nui::val event){
                        Nui::WebApi::Console::debug("item-click", event);
                        reformatColor(event["detail"]["color"].as<std::string>());
                        onChange_();
                    }
                }()
            ),
            reset(),
            help()
        );
        // clang-format on
    }

  private:
    std::weak_ptr<Nui::Dom::BasicElement> colorPopover{};
};