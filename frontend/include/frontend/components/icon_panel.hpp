#pragma once

#include <frontend/components/ui5/icon.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <fmt/format.h>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/class.hpp>
#include <nui/frontend/attributes/style.hpp>

struct IconPanelOptions
{
    std::string name;
    std::string color;
    int padding = 12;
    bool withBorder = false;
    int colorMixinPercent = 50;
};
inline Nui::ElementRenderer iconPanel(IconPanelOptions const& options)
{
    using Nui::Elements::div;
    using Nui::Attributes::class_;
    using Nui::Attributes::style;
    using Nui::Attributes::Style;
    using namespace Nui::Attributes::Literals;

    return div{
        class_ = "icon-panel",
        style = Style{
            "background-color"_style = fmt::format(
                "color-mix(in srgb, {} {}%, black {}%)",
                options.color,
                options.colorMixinPercent,
                100 - options.colorMixinPercent
            ),
            "border"_style = [withBorder = options.withBorder]() -> std::optional<std::string>
            {
                if (withBorder)
                    return "1px solid var(--sapNeutralBorderColor)";
                return std::nullopt;
            }(),
            "padding"_style = fmt::format("{}px", options.padding),
        }
    }(ui5::icon{"name"_prop = options.name}());
}