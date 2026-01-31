#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

namespace Components::Svg
{
    inline Nui::ElementRenderer deleteIcon()
    {
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;

        // clang-format off
        return svg::svg {
            svga::viewBox = "0 0 24 24",
            svga::fill = "none",
        }(
            svg::path {
                svga::d =
                    "M3 6h18"
                    "M8 6V4h8v2"
                    "M6 6l1 14h10l1-14"
                    "M10 11v6"
                    "M14 11v6",
                svga::stroke = "currentColor",
                svga::strokeWidth = "1.6",
                svga::strokeLinecap = "round",
                svga::strokeLinejoin = "round"
            }()
        );
        // clang-format on
    }
}