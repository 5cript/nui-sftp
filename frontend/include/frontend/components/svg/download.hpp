#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

namespace Components::Svg
{
    inline Nui::ElementRenderer download()
    {
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;

        // clang-format off
        return svg::svg {
            svga::viewBox = "0 0 24 24",
            svga::fill = "none",
        }(
            svg::path {
                svga::d = "M12 3v10m0 0 4-4m-4 4-4-4M5 21h14",
                svga::stroke = "currentColor",
                svga::strokeWidth = "1.6",
                svga::strokeLinecap = "round",
                svga::strokeLinejoin = "round"
            }()
        );
        // clang-format on
    }
}