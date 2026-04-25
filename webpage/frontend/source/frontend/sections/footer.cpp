#include <frontend/sections/footer.hpp>

#include <frontend/brand_mark.hpp>
#include <frontend/utf8.hpp>

#include <version.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <fmt/format.h>

#include <string>

namespace NuiSftpPage::Sections
{
    namespace
    {
        Nui::ElementRenderer brandBlock()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            return div{class_ = "brand-block"}(
                div{
                    class_ = "brand"
                }(div{class_ = "brand-mark lg"}(brandMarkSvg()),
                    span{style = "font-weight: 700; font-size: 18px"}("NuiSftp")),
                p{}("A modern SFTP & SSH workbench. Free and open source. "
                    "Built for people who live at the terminal.")
            );
        }

        Nui::ElementRenderer productColumn()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            return div{}(
                h6{}("Product"),
                a{href = "#features"}("Features"),
                a{href = "#platforms"}("Download"),
                a{href = "#docs"}("Documentation"),
                a{href = "https://github.com/5cript/nui-sftp/releases"}("Changelog")
            );
        }

        Nui::ElementRenderer sourceColumn()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            return div{}(
                h6{}("Source"),
                a{
                    href = "https://github.com/5cript/nui-sftp",
                    target = "_blank",
                    rel = "noreferrer",
                }("GitHub"),
                a{
                    href = "https://github.com/5cript/nui-sftp/issues",
                    target = "_blank",
                    rel = "noreferrer",
                }("Issues"),
                a{
                    href = "https://github.com/5cript/nui-sftp/releases",
                    target = "_blank",
                    rel = "noreferrer",
                }("Releases"),
                a{
                    href = "https://github.com/5cript/nui-sftp/blob/main/LICENSE",
                    target = "_blank",
                    rel = "noreferrer",
                }("License")
            );
        }

        Nui::ElementRenderer communityColumn()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            return div{}(
                h6{}("Community"),
                a{href = "https://github.com/5cript/nui-sftp/discussions"}("Discussions"),
                a{href = "https://github.com/5cript/nui-sftp/issues"}("Contributing")
            );
        }
    }

    Nui::ElementRenderer footer()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        auto const dot = Utf8::cp(0x00B7);

        return Nui::Elements::footer{}(div{
            style = "width: 100%;"
                    " display: flex;"
                    " flex-direction: column;"
                    " align-items: center",
        }(div{class_ = "foot"}(brandBlock(), productColumn(), sourceColumn(), communityColumn()),
            div{
                class_ = "foot-bottom"
            }(span{}(fmt::format("// nui-sftp {0} v{1} {0} 2026", dot, VERSION_NON_DIRTY)),
                span{}(std::string{"built with care "} + dot + " permissive license"))));
    }
}
