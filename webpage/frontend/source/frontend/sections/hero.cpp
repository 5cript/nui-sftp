#include <frontend/sections/hero.hpp>

#include <frontend/screenshot_url.hpp>
#include <frontend/utf8.hpp>

#include <version.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/val.hpp>

#include <fmt/format.h>

namespace NuiSftpPage::Sections
{
    namespace
    {
        /**
         * @brief 3D-tilted application mockup (formerly the design's <AppMock />).
         *
         * Structure mirrors the design verbatim so the existing
         * .mock-wrap / .mock-shot* / .chip-float CSS rules apply unchanged.
         * The screenshot is session.png from the S4 bucket, see screenshotUrl().
         */
        Nui::ElementRenderer appMock()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            auto const arrows = Utf8::cps({0x2191, 0x2193}); // up + down arrow

            return div{
                class_ = "mock-wrap"
            }(div{class_ = "mock-glow"}(),
                div{
                    class_ = "mock-shot",
                    onClick = [](Nui::val event) {
                        auto const img = event["currentTarget"].call<Nui::val>("querySelector", std::string{"img"});
                        if (!img.isNull() && !img.isUndefined())
                            Nui::val::global("nuiSftpLightboxOpen")(img);
                    },
                }(div{class_ = "mock-shot-frame"}(
                      img{
                          src = screenshotUrl("session.png"),
                          alt = "NuiSftp application screenshot",
                      }(),
                      div{class_ = "mock-shot-sheen"}()
                  ),
                    div{class_ = "mock-shot-floor"}()),
                div{class_ =
                        "chip-float tl"}(span{class_ = "pulse"}(), span{}("uploads on save")),
                div{
                    class_ = "chip-float br"
                }(span{style = "color: var(--accent-b)"}(arrows), span{}("shells + SFTP, one connection")));
        }
    }

    Nui::ElementRenderer hero()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;
        using Nui::Elements::section;

        auto const dot = Utf8::cp(0x00B7); // ·
        auto const arrowDown = Utf8::cp(0x2193); // ↓

        // clang-format off
        return section{class_ = "hero"}(
            div{class_ = "hero-inner"}(
                div{}(
                    div{class_ = "eyebrow"}(
                        span{class_ = "dot"}(),
                        span{}(
                            fmt::format("v{0} {1} Windows {1} Linux {1} macOS {1} MIT license", VERSION_NON_DIRTY, dot)
                        )
                    ),
                    h1{class_ = "hero-title"}(
                        text{"A modern "}(),
                        span{class_ = "grad"}("SFTP & SSH"),
                        text{" workbench."}()
                    ),
                    p{class_ = "hero-sub"}(
                        "Terminals and file transfers share one SSH connection. "
                        "Synchronize folders, edit remote files with your local apps, "
                        "and keep the commands you use. Free and open source."
                    ),
                    div{class_ = "hero-actions"}(
                        a{class_ = "btn primary shine", href = "#platforms"}(
                            span{}(fmt::format("{} Download", arrowDown))
                        ),
                        a{
                            class_ = "btn ghost",
                            href = "https://github.com/5cript/nui-sftp",
                            target = "_blank",
                            rel = "noreferrer"
                        }(
                            span{}("View source")
                        )
                    )
                ),
                appMock()
            )
        );
        // clang-format on
    }
}
