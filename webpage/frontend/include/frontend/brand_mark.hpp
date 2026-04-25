#pragma once

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/svg_attributes.hpp>
#include <nui/frontend/svg_elements.hpp>

namespace NuiSftpPage
{
    /**
     * @brief Inline NuiSftp brand mark -- one path + one polygon, both filled
     * with currentColor so the parent's `color:` controls the rendered tint
     * (and any `filter: drop-shadow(...)` on the parent applies cleanly).
     *
     * Used by the nav and the footer. Path data is verbatim from the design's
     * LogoMark SVG.
     */
    inline Nui::ElementRenderer brandMarkSvg()
    {
        namespace SvgEl = Nui::Elements::Svg;
        namespace SvgAttr = Nui::Attributes::Svg;
        using namespace Nui::Attributes;

        return SvgEl::svg{
            SvgAttr::viewBox = "0 0 500 500",
            "aria-hidden"_attr = "true",
        }(
            SvgEl::g{
                SvgAttr::transform = "matrix(1.4755152,0,0,1.4755152,-118.95322,-118.94235)",
                SvgAttr::fill = "currentColor",
            }(
                SvgEl::path{
                    SvgAttr::d = "m 364.3,293.5 -0.3,0.4 -26.6,31.8 -22.6,26.8 -22.6,-26.8 "
                                 "-26.6,-31.8 -0.3,-0.4 h 29 v -28.2 c 0.1,0 0.1,0 0.1,0 "
                                 "l 0.2,-31.8 c -0.1,-3.2 -0.2,-6.4 -0.5,-9.7 -1.8,-26 "
                                 "-24.3,-39.5 -47.2,-37.1 -25,2.5 -39.7,20.2 -41,46.3 "
                                 "0,0.6 0,1.2 0,1.9 0,0.9 0,1.8 0,2.7 L 202,233 192.9,222.4 "
                                 "c -0.2,-0.2 -0.3,-0.4 -0.6,-0.6 l -7.3,-8.7 -11.4,13.5 "
                                 "-9.1,10.8 v -89.9 h 41.3 v 24.1 c 13.5,-17.2 33.5,-24.3 "
                                 "55.6,-24.1 46.7,0.4 73,36.3 73.3,80.9 v 4.6 c 0,0 0.1,7.5 "
                                 "0.1,7.5 v 24.8 c 0.1,0 0.3,28.2 0.3,28.2 h 29 z",
                }(),
                SvgEl::polygon{
                    SvgAttr::points = "164.7,292 135.7,292 136,291.6 162.6,259.8 185.1,233 "
                                      "207.7,259.8 234.2,291.6 234.5,292 205.5,292 205.4,320.1 "
                                      "205.2,352.5 185.1,352.3 165,352.5 164.9,320.1",
                }()
            )
        );
    }
}
