#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

namespace Components::Svg
{
    inline Nui::ElementRenderer scan()
    {
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;
        using namespace Nui::Attributes;

        // clang-format off
        return svg::svg {
            svga::viewBox = "0 0 24 24",
            svga::fill = "none",
        }(
            // Circle (lens)
            svg::circle {
                svga::cx = "11",
                svga::cy = "11",
                svga::r = "7",
                svga::stroke = "currentColor",
                svga::strokeWidth = "1.6",
            }(),
            // Handle
            svg::line {
                "x1"_attr = "16.65",
                "y1"_attr = "16.65",
                "x2"_attr = "21",
                "y2"_attr = "21",
                svga::stroke = "currentColor",
                svga::strokeWidth = "1.6",
                svga::strokeLinecap = "round",
            }()
        );
        // clang-format on
    }
}