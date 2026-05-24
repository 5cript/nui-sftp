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
        // `copyText` makes the row click-to-copy via globalThis.nuiSftpCopyToClipboard.
        Nui::ElementRenderer
        pkgRow(std::string const& label, std::string const& kind, bool comingSoon = false, std::string copyText = {})
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            std::string classes = "pkg-row";
            if (!copyText.empty())
                classes += " copyable";

            auto const styleStr = comingSoon ? std::string{"opacity: 0.6"} : std::string{};

            if (copyText.empty())
            {
                return div{
                    class_ = classes,
                    style = styleStr,
                }(span{}(label), span{class_ = "sep"}(), span{class_ = "copy"}(kind));
            }

            return div{
                class_ = classes,
                style = styleStr,
                Nui::Attributes::title = "Click to copy",
                onClick =
                    [copyText](Nui::val event) {
                        Nui::val::global("nuiSftpCopyToClipboard")(event["currentTarget"], copyText);
                    },
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

            std::string const description =
                "Pick the installer for a Start Menu entry and clean in-place upgrades, "
                "or grab the portable zip and unpack anywhere.";

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
                pkgRow("nui-sftp-setup.exe", "installer"),
                pkgRow("nui-sftp-portable.zip", "portable"),
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

            // "AUR · Flathub · AppImage today · NixOS soon" — middle dot U+00B7
            std::string const dot = " " + Utf8::cp(0x00B7) + " ";
            std::string const versionLine = "AUR" + dot + "Flathub" + dot + "AppImage today" + dot + "NixOS soon";

            std::string const description =
                "Native AUR package, an AppImage from Releases, and an official Flatpak on Flathub today. "
                "nixpkgs is on the way.";

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
                pkgRow("yay -S nui-sftp", "archlinux user repository", false, "yay -S nui-sftp"),
                pkgRow("./nui-sftp-*.AppImage", "appimage"),
                pkgRow(
                    "flatpak install flathub org.nuicpp.nui_sftp",
                    "flathub",
                    false,
                    "flatpak install flathub org.nuicpp.nui_sftp"),
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
                h2{class_ = "section-title"}("Native on Windows. AUR on Arch. Flathub & AppImage everywhere else."),
                p{class_ = "section-sub"}()),
            div{class_ = "platforms"}(windowsCard(), linuxCard())
        ));
    }
}
