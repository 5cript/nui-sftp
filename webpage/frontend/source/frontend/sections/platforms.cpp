#include <frontend/sections/platforms.hpp>

#include <frontend/utf8.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <string>

namespace NuiSftpPage::Sections
{
    namespace
    {
        // A single distribution row: <package label> · · · <kind tag>
        // `comingSoon` dims the row to mark not-yet-available distros.
        Nui::ElementRenderer pkgRow(std::string const& label, std::string const& kind, bool comingSoon = false)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            return div{
                class_ = "pkg-row",
                style = comingSoon ? "opacity: 0.6" : "",
            }(span{}(label), span{class_ = "sep"}(), span{class_ = "copy"}(kind));
        }

        Nui::ElementRenderer windowsCard()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            // "Windows 10 · 11 · x86_64" — middle dot U+00B7
            std::string const versionLine = "Windows 10 " + Utf8::cp(0x00B7) + " 11 " + Utf8::cp(0x00B7) + " x86_64";

            std::string const description = "Download and unpack in the location of your choice.";

            return div{class_ = "platform-card glass shine"}(
                div{
                    style = "display:flex; align-items:center; gap:14px"
                }(div{class_ = "platform-glyph"}(Utf8::cp(0x229E)), // ⊞
                    div{}(
                        h3{}("Windows"),
                        div{
                            style = "font-family: 'JetBrains Mono', monospace;"
                                    " font-size: 12px;"
                                    " color: var(--ink-muted);"
                                    " margin-top: 2px",
                        }(versionLine)
                    )),
                p{style = "margin: 0; color: var(--ink-dim); font-size: 14px"}(description),
                pkgRow("nui-sftp-portable.zip", ".zip"),
                pkgRow("nui-sftp-setup.exe", ".exe", true),
                a{
                    class_ = "btn primary shine",
                    href = "https://github.com/5cript/nui-sftp/releases",
                    target = "_blank",
                    rel = "noreferrer",
                    style = "margin-top: 8px",
                }(span{}("Get the Windows build"))
            );
        }

        Nui::ElementRenderer linuxCard()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            // "AUR today · Flathub & NixOS soon" — middle dot U+00B7
            std::string const versionLine = "AUR today " + Utf8::cp(0x00B7) + " Flathub & NixOS soon";

            std::string const description =
                "Native AUR package and a Flatpak bundle from Releases today. "
                "Flathub and nixpkgs are on the way.";

            return div{class_ = "platform-card glass shine"}(
                div{
                    style = "display:flex; align-items:center; gap:14px"
                }(div{class_ = "platform-glyph"}(Utf8::cp(0x1F427)), // 🐧
                    div{}(
                        h3{}("GNU/Linux"),
                        div{
                            style = "font-family: 'JetBrains Mono', monospace;"
                                    " font-size: 12px;"
                                    " color: var(--ink-muted);"
                                    " margin-top: 2px",
                        }(versionLine)
                    )),
                p{style = "margin: 0; color: var(--ink-dim); font-size: 14px"}(description),
                pkgRow("yay -S nui-sftp", "archlinux user repository"),
                pkgRow("flatpak install ./nui-sftp-*.flatpak", "from releases"),
                pkgRow("flathub: org.nuicpp.nui_sftp", "soon", true),
                pkgRow("nixpkgs.nui-sftp", "soon", true),
                a{
                    class_ = "btn primary shine",
                    href = "https://github.com/5cript/nui-sftp/releases",
                    target = "_blank",
                    rel = "noreferrer",
                    style = "margin-top: 8px",
                }(span{}("Download from Releases"))
            );
        }
    }

    Nui::ElementRenderer platforms()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::section;

        return section{class_ = "section", id = "platforms"}(div{class_ = "section-inner"}(
            div{
                class_ = "section-head"
            }(div{class_ = "section-eyebrow"}("// download"),
                h2{class_ = "section-title"}("Native on Windows. AUR on Arch. Flathub & NixOS coming."),
                p{class_ = "section-sub"}()),
            div{class_ = "platforms"}(windowsCard(), linuxCard())
        ));
    }
}
