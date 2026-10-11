#include <frontend/sections/open_source.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <string>

namespace NuiSftpPage::Sections
{
    namespace
    {
        // Reusable inline pill style — small rounded chip with glass-like fill
        // and subtle border. Used for the topic tags on the repo card.
        constexpr char const* kRepoTagStyle =
            "padding: 3px 8px;"
            " border-radius: 999px;"
            " background: rgba(255, 255, 255, 0.05);"
            " border: 1px solid var(--line)";

        Nui::ElementRenderer ossCopy()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            return div{
                class_ = "oss-copy"
            }(div{class_ = "section-eyebrow"}("// open source"),
                h2{}("Open source"),
                p{}("The full source is on GitHub under the MIT license. Bug reports, ideas and pull requests are welcome."),
                div{class_ = "oss-actions"}(
                    a{
                        class_ = "btn primary shine",
                        href = "https://github.com/5cript/nui-sftp",
                        target = "_blank",
                        rel = "noreferrer",
                    }(span{}("github.com/5cript/nui-sftp")),
                    a{
                        class_ = "btn ghost",
                        href = "https://github.com/5cript/nui-sftp/issues",
                        target = "_blank",
                        rel = "noreferrer",
                    }(span{}("Report an issue"))
                ));
        }

        Nui::ElementRenderer repoCard()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;
            using Nui::Elements::b;

            return div{
                class_ = "repo-card"
            }(div{class_ = "repo-title"}(text{"5cript / "}(), span{}("nui-sftp")),
                div{
                    style = "color: var(--ink-dim);"
                            " font-family: 'Inter', sans-serif;"
                            " font-size: 13px;"
                            " line-height: 1.5",
                }("A cross platform ssh and sftp client"),
                div{
                    class_ = "repo-meta"
                }(span{}(b{}("C++")), span{}(b{}("MIT"), text{std::string_view{" license"}}())),
                div{
                    style = "display: flex;"
                            " gap: 8px;"
                            " margin-top: 4px;"
                            " font-size: 11px",
                }(span{style = kRepoTagStyle}("sftp"),
                    span{style = kRepoTagStyle}("ssh"),
                    span{style = kRepoTagStyle}("cpp")));
        }
    }

    Nui::ElementRenderer openSource()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::section;

        return section{
            class_ = "section", id = "oss"
        }(div{class_ = "section-inner"}(div{class_ = "oss glass"}(ossCopy(), div{class_ = "oss-visual"}(repoCard()))));
    }
}
