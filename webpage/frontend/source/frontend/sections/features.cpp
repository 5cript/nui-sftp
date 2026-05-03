#include <frontend/sections/features.hpp>

#include <frontend/utf8.hpp>

#include <script-nui-components/carousel.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/event_system/range.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/val.hpp>

#include <fmt/format.h>

#include <cctype>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace NuiSftpPage::Sections
{
    namespace
    {
        struct Feature
        {
            std::string eyebrow;
            std::string title;
            std::string desc;
            std::vector<std::string> tags;
            // Optional path under /assets for the product screenshot. Empty
            // means the slide drops the visual side entirely and the copy
            // stretches across the full slide width.
            std::string visualImage{};
        };

        // Mirrors the FEATURES array from the design mockup. The visual side
        // of each slide is intentionally a placeholder; product screenshots
        // will drop in later.
        std::vector<Feature> const& featureList()
        {
            // Initialise via IIFE so we can declare runtime-built UTF-8 glyphs
            // once and splice them into the description / tag strings without
            // ever putting a non-ASCII byte in the source.
            static std::vector<Feature> const data = []()
            {
                auto const dot = Utf8::cp(0x00B7); // middle dot
                auto const lq = Utf8::cp(0x201C); // left double quote
                auto const rq = Utf8::cp(0x201D); // right double quote

                std::string const tagBashEtc = fmt::format("bash {0} zsh {0} fish {0} pwsh", dot);

                return std::vector<Feature>{
                    {
                        "sync",
                        "Synchronization you can see before you commit to it",
                        "Compare directories side-by-side and reconcile differences with a "
                        "single click. NuiSftp tracks what's added, modified, and removed "
                        "(locally and remotely) so your tree stays in lockstep without surprises.",
                        {"Bidirectional", "Diff preview", "Selective merge"},
                        "assets/synchronize.png",
                    },
                    {
                        "live_edit",
                        "Edit locally, deploy instantly",
                        "Open any remote file in your editor of choice. NuiSftp watches it on "
                        "disk and pushes changes back automatically the moment you save "
                        "-> no drag, no drop, no friction.",
                        {"fs.watch", "Auto-reupload", "Conflict guard"},
                        "assets/live_edit.png",
                    },
                    {
                        "ssh_shells",
                        "SSH shells alongside SFTP, multiplexed",
                        "Open as many interactive shell channels as your server allows, "
                        "all over the same connection that's transferring your files. Switch "
                        "between sessions in tabs, never juggle terminals again.",
                        {"Multiple channels", "Per-session tabs", "Same connection"},
                        "assets/session.png",
                    },
                    {
                        "configurable",
                        "Everything is Configurable",
                        "Profiles per host, themable panels, layout presets, reuseable inheritable settings. "
                        "If you want to bend it to your workflow, "
                        "NuiSftp gets out of the way.",
                        {"Profiles", "Themes", "Hooks", "Keybinds"},
                        "assets/settings.png",
                    },
                    {
                        "local_terminal",
                        "Local terminals, right where you need them",
                        "A full local shell lives next to your remote panes. Run a build, scrub a "
                        "log, or git-commit without ever leaving the workbench.",
                        {tagBashEtc, "Split panes", "Persistent history"},
                        "assets/local.png",
                    },
                    {
                        "free",
                        "Free. No accounts, no telemetry, no nags.",
                        fmt::format(
                            "Download it. Use it forever. No ads, no telemetry."
                            "The whole tool, all the features, available to everyone.",
                            lq,
                            rq
                        ),
                        {"Zero cost", "Zero tracking"},
                    },
                    {
                        "open_source",
                        "Open source. Read it, fork it, ship it.",
                        "Every line of NuiSftp is on GitHub under a permissive license. Based on "
                        "libssh and the NuiCpp UI framework, both of which are open source as well.",
                        {"MIT-style", "Pull requests welcome", "Transparent"},
                    },
                    {
                        "cross_platform",
                        "Native on Windows. Flatpak on Linux. Native on Arch. NixOS & AppImage & Ubuntu 26 coming.",
                        "First-class Windows builds, distro-agnostic Flatpak today, and native "
                        "nixpkgs packages on the way. Same UI, same configuration, same "
                        "keybinds.",
                        {"Windows 10/11", "AUR", "Flatpak", "AppImage Soon"},
                    },
                };
            }();
            return data;
        }

        std::string abridgedTitle(std::string const& title)
        {
            static auto const sep = std::string{",."} + Utf8::cp(0x2014);
            auto const pos = title.find_first_of(sep);
            std::string clause = (pos == std::string::npos) ? title : title.substr(0, pos);
            while (!clause.empty() && std::isspace(static_cast<unsigned char>(clause.back())))
                clause.pop_back();
            return clause;
        }

        // First two tags joined by " <middle-dot> ".
        std::string firstTwoTags(std::vector<std::string> const& tags)
        {
            if (tags.empty())
                return {};
            if (tags.size() == 1)
                return tags.front();
            return fmt::format("{} {} {}", tags[0], Utf8::cp(0x00B7), tags[1]);
        }

        Nui::ElementRenderer renderSlide(Feature const& feature)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            // Owning range over a copied tag vector — the rvalue overload of
            // Nui::range takes ownership so the strings stay alive for the
            // lifetime of the rendered element.
            auto copy = div{class_ = "slide-copy"}(
                div{class_ = "slide-num"}(feature.eyebrow),
                h3{class_ = "slide-title"}(feature.title),
                p{class_ = "slide-desc"}(feature.desc),
                div{class_ = "slide-tags"}(
                    // Parenthesised copy-construction (brace-init can be
                    // misread as an initializer_list of strings).
                    Nui::range(std::vector<std::string>(feature.tags)),
                    [](long long, std::string const& tag) -> Nui::ElementRenderer
                    {
                        using namespace Nui::Elements;
                        using namespace Nui::Attributes;
                        return span{class_ = "slide-tag"}(tag);
                    }
                )
            );

            if (feature.visualImage.empty())
                return div{class_ = "carousel-slide carousel-slide--copy-only"}(std::move(copy));

            return div{
                class_ = "carousel-slide"
            }(std::move(copy),
                div{class_ = "slide-visual"}(img{
                    class_ = "slide-visual-image",
                    src = feature.visualImage,
                    alt = feature.title,
                    onClick = [](Nui::val event)
                    {
                        // Lightbox JS lives in static/source/lightbox.js
                        // and registers itself on globalThis.
                        Nui::val::global("nuiSftpLightboxOpen")(event["currentTarget"]);
                    },
                }()));
        }
    }

    namespace
    {
        // Builds a single .feature-mini recap card; clicking it jumps the
        // carousel to the matching slide, and the active class tracks page.
        Nui::ElementRenderer
        renderRecapCard(std::shared_ptr<Nui::Observed<int>> const& page, int index, Feature const& feature)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            return div{
                class_ = Nui::observe(*page).generate(
                    [index](int currentPage) -> std::string
                    {
                        return currentPage == index ? "feature-mini active" : "feature-mini";
                    }
                ),
                onClick = [page, index](Nui::val)
                {
                    *page = index;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
            }(div{class_ = "ix"}(feature.eyebrow), h4{}(abridgedTitle(feature.title)), p{}(firstTwoTags(feature.tags)));
        }

        // The full .features-grid recap row beneath the carousel — one mini
        // card per feature, range-driven so the count tracks featureList().
        Nui::ElementRenderer renderRecapGrid(std::shared_ptr<Nui::Observed<int>> const& page)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            auto const count = static_cast<int>(featureList().size());
            std::vector<int> indices(static_cast<std::size_t>(count));
            std::iota(indices.begin(), indices.end(), 0);

            return div{
                class_ = "features-grid",
                style = "margin-top: 28px",
            }(Nui::range(std::move(indices)),
                [page](long long, int const& i) -> Nui::ElementRenderer
                {
                    auto const& list = featureList();
                    if (i < 0 || i >= static_cast<int>(list.size()))
                        return Nui::nil();
                    return renderRecapCard(page, i, list[i]);
                });
        }
    }

    struct Features::Implementation
    {
        std::shared_ptr<Nui::Observed<int>> page;
        ScriptNuiComponents::Carousel carousel;

        Implementation()
            : page{std::make_shared<Nui::Observed<int>>(0)}
            , carousel{
                  page,
                  [](int index) -> Nui::ElementRenderer
                  {
                      auto const& list = featureList();
                      if (index < 0 || index >= static_cast<int>(list.size()))
                          return Nui::nil();
                      return renderSlide(list[index]);
                  },
                  static_cast<int>(featureList().size()),
              }
        {}
    };

    Features::Features()
        : impl_{std::make_unique<Implementation>()}
    {}
    Features::~Features() = default;
    Features::Features(Features&&) = default;
    Features& Features::operator=(Features&&) = default;

    Nui::ElementRenderer Features::operator()()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::section;

        return section{class_ = "section", id = "features"}(div{
            class_ = "section-inner"
        }(div{class_ = "section-head"}(
              div{class_ = "section-eyebrow"}("// what's inside"),
              h2{class_ = "section-title"}("Eight things that make file work feel less like file work."),
              p{class_ = "section-sub"}("A tour of the features that ship today. Missing something you'd like to see? "
                                        "Open an issue or pull request, suggestions are always welcome.")
          ),
            impl_->carousel(),
            renderRecapGrid(impl_->page)));
    }
}
