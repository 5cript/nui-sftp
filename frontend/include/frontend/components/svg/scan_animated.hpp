#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

namespace Components::Svg
{
    inline Nui::ElementRenderer scanAnimated()
    {
        namespace svg = Nui::Elements::Svg;
        namespace svga = Nui::Attributes::Svg;
        using namespace Nui::Attributes;

        // clang-format off
        return svg::svg {
            svga::viewBox = "0 0 24 24",
            svga::fill = "none",
            "xmlns"_attr = "http://www.w3.org/2000/svg"
        }(
            // Lens (circle)
            svg::circle {
                svga::cx = "11",
                svga::cy = "11",
                svga::r = "7",
                svga::stroke = "currentColor",
                svga::strokeWidth = "1.6",
                svga::strokeLinecap = "round",
                svga::strokeLinejoin = "round"
            }(),

            // Moving scan line inside the lens (thin rounded rect)
            svg::rect {
                // place rectangle inside circle bounds (left aligned at x=5 to right at x=17)
                svga::x = "5",
                svga::y = "6",           // starting y — animate will update this
                svga::width = "12",
                svga::height = "0.9",
                svga::rx = "0.45",      // rounded corners to make it look like a soft line
                svga::fill = "currentColor",
                svga::opacity = "0.18",
                svga::clipPath = "url(#lensClip)" // clip to the lens so it doesn't draw outside
            }(
                // animate y position: 6 -> 16 -> 6 (loop)
                svg::animate {
                    svga::attributeName = "y",
                    svga::values = "6;16;6",
                    svga::dur = "1.6s",
                    svga::repeatCount = "indefinite",
                    svga::keyTimes = "0;0.5;1",
                    svga::calcMode = "linear"
                }()
            ),

            // optional faint pulse on the leading edge to suggest scanning sweep (subtle)
            svg::rect {
                svga::x = "5",
                svga::y = "6",
                svga::width = "12",
                svga::height = "0.9",
                svga::rx = "0.45",
                svga::fill = "currentColor",
                svga::opacity = "0.0",
                svga::clipPath = "url(#lensClip)"
            }(
                svg::animate {
                    svga::attributeName = "opacity",
                    svga::values = "0;0.28;0",
                    svga::dur = "1.6s",
                    svga::repeatCount = "indefinite",
                    svga::keyTimes = "0;0.5;1"
                }(),
                // animate the opacity slightly offset (so the pulse follows the scan line)
                svg::animateTransform {
                    svga::attributeName = "transform",
                    svga::type = "translate",
                    svga::values = "0 0; 0 10; 0 0",
                    svga::dur = "1.6s",
                    svga::repeatCount = "indefinite"
                }()
            ),

            // Clip path matching the lens so scan line stays inside the circle
            svg::defs {}(
                svg::clipPath {
                    svga::id = "lensClip"
                }(
                    svg::circle {
                        svga::cx = "11",
                        svga::cy = "11",
                        svga::r = "7"
                    }()
                )
            ),

            // Handle
            svg::line {
                "x1"_attr = "16.65",
                "y1"_attr = "16.65",
                "x2"_attr = "21",
                "y2"_attr = "21",
                "stroke"_attr = "currentColor",
                "strokeWidth"_attr = "1.6",
                "strokeLinecap"_attr = "round",
                "strokeLinejoin"_attr = "round"
            }()
        );
        // clang-format on
    }
}