#include <frontend/sections/features.hpp>

#include <frontend/screenshot_url.hpp>
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
            // Optional URL of the product screenshot. Empty means the slide
            // drops the visual side entirely and the copy stretches across the
            // full slide width.
            std::string visualImage{};
        };

        // Mirrors the FEATURES array from the design mockup.
        std::vector<Feature> const& featureList()
        {
            // Initialise via IIFE so we can declare runtime-built UTF-8 glyphs
            // once and splice them into the description / tag strings without
            // ever putting a non-ASCII byte in the source.
            static std::vector<Feature> const data = []()
            {
                auto const dot = Utf8::cp(0x00B7); // middle dot

                std::string const tagShells = fmt::format("bash {0} zsh {0} fish", dot);

                return std::vector<Feature>{
                    {
                        "sync",
                        "Folder synchronization",
                        "Compare a local and a remote folder and review the differences before anything is "
                        "transferred. Pick the files to upload, download or delete, sync in both directions or "
                        "only one, and leave out hidden files or whatever .gitignore lists.",
                        {"Preview", "Per-file selection", "One or both directions"},
                        screenshotUrl("synchronize.png"),
                    },
                    {
                        "live_edit",
                        "Edit remote files with local apps",
                        "Open a remote file in its default application or pick another one. NuiSftp watches "
                        "the local copy and uploads it again every time you save.",
                        {"Open and watch", "Upload on save"},
                        screenshotUrl("live_edit.png"),
                    },
                    {
                        "ssh_shells",
                        "Shells and SFTP on one connection",
                        "Open as many shell channels as the server allows, on the same SSH connection that "
                        "transfers your files. Each session has its own tab.",
                        {"Multiple channels", "Session tabs", "One connection"},
                        screenshotUrl("session.png"),
                    },
                    {
                        "history_snippets",
                        "Command history and snippets",
                        "Commands you run in a terminal are recorded per host, so you can search them, sort them "
                        "by use and run them again. Commands you need often go into snippet folders, with "
                        "{{placeholders}} you fill in when you run them. 27 ready-made folders cover Docker, Git, "
                        "systemd, Kubernetes, package managers and more.",
                        {"Per-host history", "Placeholders", "Preset folders"},
                        screenshotUrl("snippets.png"),
                    },
                    {
                        "configurable",
                        "Configurable",
                        "Servers inherit their settings from named defaults and override only what differs. "
                        "Layouts can be saved per server, a search finds any setting, and there are several "
                        "themes with a light and a dark mode.",
                        {"Inherited defaults", "Saved layouts", "Settings search", "Themes"},
                        screenshotUrl("settings.png"),
                    },
                    {
                        "local_terminal",
                        "Local terminals",
                        "Local shells run next to your remote sessions, with a file view of your own machine. "
                        "The shell command is configurable, so bash, zsh, fish or anything else works.",
                        {tagShells, "Dockable panels", "Command history"},
                        screenshotUrl("local.png"),
                    },
                    {
                        "free_and_open",
                        "Free and open source",
                        "No accounts, no ads, no telemetry. The source is on GitHub under the MIT license. "
                        "NuiSftp is built on libssh and the NuiCpp UI framework, both open source as well.",
                        {"No accounts", "No telemetry", "MIT license"},
                    },
                    {
                        "cross_platform",
                        "Cross-platform",
                        "An installer and a portable zip for Windows, packages on the AUR and Flathub, an AppImage "
                        "for other distributions, and a disk image for Apple Silicon Macs. The app works the same "
                        "on all of them.",
                        {"Windows 10/11", "Linux", "macOS 15.2+"},
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
              div{class_ = "section-eyebrow"}("// features"),
              h2{class_ = "section-title"}("What's in the app"),
              p{class_ = "section-sub"}("The main features of the current release. Missing something? "
                                        "Open an issue or a pull request.")
          ),
            impl_->carousel(),
            renderRecapGrid(impl_->page)));
    }
}
