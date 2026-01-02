#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

namespace Components::Svg
{
    inline Nui::ElementRenderer pause()
    {
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;

        // clang-format off
        return svg::svg {
            svga::viewBox = "0 0 24 24",
            svga::fill = "none",
        }(
            svg::path {
                svga::d = "M6 5h4v14H6zM14 5h4v14h-4z",
                svga::fill = "currentColor"
            }()
        );
        // clang-format on
    }
}