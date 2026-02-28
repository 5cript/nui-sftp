#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <fmt/format.h>

#include <nui/frontend/elements/div.hpp>
#include <nui/frontend/attributes/class.hpp>
#include <nui/frontend/attributes/style.hpp>

struct IconPanelOptions
{
    Nui::ElementRenderer icon;
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
    using namespace Nui::Attributes::Literals;

    return div{
        class_ = "icon-panel",
        style = fmt::format(
            "background-color: color-mix(in srgb, {} {}%, black {}%);{}padding: {}px;",
            options.color,
            options.colorMixinPercent,
            100 - options.colorMixinPercent,
            options.withBorder ? "border: 1px solid var(--sapNeutralBorderColor);" : "",
            options.padding
        ),
    }(std::move(options.icon));
}