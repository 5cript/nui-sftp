#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

namespace Components::Svg
{
    inline Nui::ElementRenderer cancel()
    {
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;

        // clang-format off
        return svg::svg {
            svga::viewBox = "0 0 24 24",
            svga::fill = "none",
        }(
            svg::path {
                svga::d = "M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18M9 9l6 6M15 9l-6 6",
                svga::stroke = "currentColor",
                svga::strokeWidth = "1.8",
                svga::strokeLinecap = "round"
            }()
        );
        // clang-format on
    }
}
