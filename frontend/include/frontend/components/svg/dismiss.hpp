#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

namespace Components::Svg
{
    inline Nui::ElementRenderer dismiss()
    {
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;

        // clang-format off
        return svg::svg {
            svga::viewBox = "0 0 24 24",
            svga::fill = "none",
        }(
            svg::path {
                svga::d = "M7 7l10 10M17 7L7 17",
                svga::stroke = "currentColor",
                svga::strokeWidth = "2.5",
                svga::strokeLinecap = "round"
            }()
        );
        // clang-format on
    }
}
