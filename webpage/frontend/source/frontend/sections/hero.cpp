#include <frontend/sections/hero.hpp>

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
         * The screenshot itself is `assets/session.png`, fetched via
         * FetchContent and staged into bin/assets at build time.
         */
        Nui::ElementRenderer appMock()
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            auto const dot = Utf8::cp(0x00B7); // middle dot
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
                          src = "assets/session.png",
                          alt = "NuiSftp application screenshot",
                      }(),
                      div{class_ = "mock-shot-sheen"}()
                  ),
                    div{class_ = "mock-shot-floor"}()),
                div{class_ =
                        "chip-float tl"}(span{class_ = "pulse"}(), span{}(fmt::format("watch {} auto-reupload", dot))),
                div{
                    class_ = "chip-float br"
                }(span{style = "color: var(--accent-b)"}(arrows), span{}("SFTP + SSH multiplexed")));
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
                            fmt::format("v{0} {1} Windows {1} Linux {1} MIT-style", VERSION_NON_DIRTY, dot)
                        )
                    ),
                    h1{class_ = "hero-title"}(
                        text{"A modern "}(),
                        span{class_ = "grad"}("SFTP & SSH"),
                        text{" workbench."}()
                    ),
                    p{class_ = "hero-sub"}(
                        "Multiplex SSH and SFTP over a single connection, "
                        "synchronize directories, and watch local edits for automatic reupload. "
                        "Free and open source."
                    ),
                    div{class_ = "hero-actions"}(
                        a{class_ = "btn primary shine", href = "#platforms"}(
                            span{}(fmt::format("{} Download for Linux & Windows", arrowDown))
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
