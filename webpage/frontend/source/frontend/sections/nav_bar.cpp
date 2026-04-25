#include <frontend/sections/nav_bar.hpp>

#include <frontend/brand_mark.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

namespace NuiSftpPage::Sections
{
    Nui::ElementRenderer navBar()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        using Nui::Elements::nav;

        return div{class_ = "nav-wrap"}(
            nav{class_ = "nav glass"}(
                div{class_ = "brand"}(
                    div{class_ = "brand-mark"}(brandMarkSvg()),
                    span{}("NuiSftp")
                ),
                div{class_ = "nav-links"}(
                    a{href = "#features"}("Features"),
                    a{href = "#platforms"}("Download"),
                    a{href = "#oss"}("Open Source"),
                    a{href = "#docs"}("Docs")
                ),
                a{
                    class_ = "cta",
                    href = "https://github.com/5cript/nui-sftp",
                    target = "_blank",
                    rel = "noreferrer"
                }(
                    span{}("GitHub")
                ),
                a{class_ = "cta primary", href = "#platforms"}("Download")
            )
        );
    }
}
