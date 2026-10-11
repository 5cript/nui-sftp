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
                "The installer adds a Start menu entry and upgrades in place. "
                "The portable zip runs from any folder.";

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
                }(span{}("Windows downloads"))
            );
        }

        Nui::ElementRenderer linuxCard()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            std::string const dot = " " + Utf8::cp(0x00B7) + " ";
            std::string const versionLine = "AUR" + dot + "Flathub" + dot + "AppImage" + dot + "nixpkgs planned";

            std::string const description =
                "Install from the AUR or Flathub, or run the AppImage on any distribution with glibc 2.35 "
                "or newer. Local shells do not work inside the Flatpak sandbox.";

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
                pkgRow("yay -S nui-sftp", "aur", false, "yay -S nui-sftp"),
                pkgRow("./nui-sftp-*.AppImage", "appimage"),
                pkgRow(
                    "flatpak install flathub org.nuicpp.nui_sftp",
                    "flathub",
                    false,
                    "flatpak install flathub org.nuicpp.nui_sftp"),
                pkgRow("nixpkgs.nui-sftp", "planned", true),
                a{
                    class_ = "btn primary shine",
                    href = "https://github.com/5cript/nui-sftp/releases",
                    target = "_blank",
                    rel = "noreferrer",
                }(span{}("Linux downloads"))
            );
        }
        Nui::ElementRenderer macosCard()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            std::string const versionLine = "macOS 15.2+ " + Utf8::cp(0x00B7) + " Apple Silicon";

            std::string const description =
                "A disk image for arm64 Macs. The app is not signed with an Apple Developer ID, "
                "so macOS asks once before the first start.";

            std::string const unquarantine = "xattr -dr com.apple.quarantine /Applications/nui-sftp.app";

            return div{class_ = "platform-card glass shine"}(
                div{
                    style = "display:flex; align-items:center; gap:14px"
                }(div{class_ = "platform-glyph"}(Utf8::cp(0x2318)), // place of interest sign
                    div{}(
                        h3{}("macOS"),
                        div{
                            style = "font-family: 'JetBrains Mono', monospace;"
                                    " font-size: 12px;"
                                    " color: var(--ink-muted);"
                                    " margin-top: 2px",
                        }(versionLine)
                    )),
                p{style = "margin: 0; color: var(--ink-dim); font-size: 14px"}(description),
                pkgRow("nui-sftp-macos-arm64.dmg", "disk image"),
                pkgRow(unquarantine, "first start", false, unquarantine),
                a{
                    class_ = "btn primary shine",
                    href = "https://github.com/5cript/nui-sftp/releases",
                    target = "_blank",
                    rel = "noreferrer",
                }(span{}("macOS downloads"))
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
                h2{class_ = "section-title"}("Windows, Linux and macOS"),
                p{class_ = "section-sub"}("Packages are attached to the releases on GitHub.")),
            div{class_ = "platforms"}(windowsCard(), linuxCard(), macosCard())
        ));
    }
}
