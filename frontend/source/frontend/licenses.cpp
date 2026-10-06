#include <frontend/licenses.hpp>

#include <licenses_data.hpp>

#include <log/log.hpp>
#include <utility/language.hpp>
#include <script-nui-components/tabs.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace
{
    struct LicenseEntry
    {
        std::string name;
        std::string version;
        std::string license;
        std::string homepage;
        std::string copyright;
        std::string role;       /// "vendored", "fetched", "system", "self", "npm"
        std::string text;
    };

    std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    std::string readString(nlohmann::json const& obj, char const* key)
    {
        const auto it = obj.find(key);
        if (it == obj.end() || it->is_null())
            return {};
        return it->get<std::string>();
    }

    std::vector<LicenseEntry> parseSection(nlohmann::json const& arr, std::string const& defaultRole)
    {
        std::vector<LicenseEntry> out;
        if (!arr.is_array())
            return out;
        out.reserve(arr.size());
        for (auto const& e : arr)
        {
            LicenseEntry entry{
                readString(e, "name"),
                readString(e, "version"),
                readString(e, "license"),
                readString(e, "homepage"),
                readString(e, "copyright"),
                readString(e, "role"),
                readString(e, "text"),
            };
            if (entry.role.empty())
                entry.role = defaultRole;
            if (entry.homepage.empty())
                entry.homepage = readString(e, "url");
            out.push_back(std::move(entry));
        }
        return out;
    }
}

struct Licenses::Implementation
{
    FrontendEvents* events;
    Nui::Observed<bool> loaded{false};
    Nui::Observed<std::vector<LicenseEntry>> entries{};
    Nui::Observed<std::string> filter{};
    /// Index into `entries`, or -1 to show the "All" view.
    Nui::Observed<long long> selectedIndex{-1};
    ScriptNuiComponents::Tabs tabs{ScriptNuiComponents::Tabs::ClassNames{
        .bar = "lm-TabBar lm-TabBar-content",
        .item = "licenses-page-tab-item",
        .tab = "lm-TabBar-tab",
        .selectedTab = "lm-mod-current",
        .label = "lm-TabBar-tabLabel",
    }};
    int licensesTabId{-1};
    int aboutTabId{-1};
    Nui::Observed<int> activeTabId{-1};
    LanguageChangeListener languageListener{};

    explicit Implementation(FrontendEvents* events)
        : events{events}
    {}

    void selectTab(int tabId)
    {
        tabs.select(tabId);
        activeTabId = tabId;
    }
};

Licenses::Licenses(FrontendEvents* events)
    : impl_{std::make_unique<Implementation>(events)}
{
    impl_->licensesTabId = impl_->tabs.add(language->get("licenses", "licensesTab"), false);
    impl_->aboutTabId = impl_->tabs.add(language->get("licenses", "aboutTab"), false);
    impl_->selectTab(impl_->licensesTabId);
    impl_->tabs.onSelect(
        [this](int tabId)
        {
            impl_->activeTabId = tabId;
            return true;
        }
    );
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Licenses);

void Licenses::loadIfNeeded()
{
    if (impl_->loaded.value())
        return;

    try
    {
        const auto doc = nlohmann::json::parse(LicenseData::json);
        std::vector<LicenseEntry> merged;
        if (auto it = doc.find("cpp"); it != doc.end())
        {
            auto cpp = parseSection(*it, "cpp");
            merged.insert(merged.end(),
                          std::make_move_iterator(cpp.begin()),
                          std::make_move_iterator(cpp.end()));
        }
        if (auto it = doc.find("npm"); it != doc.end())
        {
            auto npm = parseSection(*it, "npm");
            merged.insert(merged.end(),
                          std::make_move_iterator(npm.begin()),
                          std::make_move_iterator(npm.end()));
        }
        std::stable_sort(merged.begin(), merged.end(),
            [](LicenseEntry const& a, LicenseEntry const& b) {
                return toLower(a.name) < toLower(b.name);
            });
        impl_->entries.value() = std::move(merged);
        impl_->entries.modify();
        impl_->loaded = true;
    }
    catch (std::exception const& e)
    {
        Nui::WebApi::Console::error(std::string{"Failed to parse embedded licenses: "} + e.what());
    }
}

Nui::ElementRenderer Licenses::header()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    return div{class_ = "licenses-page-header"}(
        span{class_ = "licenses-page-title"}(language->getObserved("licenses", "title")),
        button{
            class_ = "licenses-close-button",
            onClick = [this](Nui::val) {
                impl_->events->licensesOpen = false;
            },
        }(language->get("licenses", "close"))
    );
}

Nui::ElementRenderer Licenses::sidebar()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    return div{class_ = "licenses-side"}(
        input{
            type = "text",
            placeHolder = language->get("licenses", "filterPlaceholder"),
            class_ = "licenses-filter-input",
            value = impl_->filter,
            onInput = [this](Nui::val event) {
                impl_->filter = event["target"]["value"].as<std::string>();
            },
        }(),
        div{
            class_ = "licenses-side-row licenses-side-row-all",
            style = Nui::observe(impl_->selectedIndex).generate([this]() {
                return impl_->selectedIndex.value() < 0
                    ? std::string{"background-color: var(--theme-color-accent, #2a2a2a);"}
                    : std::string{};
            }),
            onClick = [this](Nui::val) {
                impl_->selectedIndex = -1;
            },
        }(
            span{class_ = "licenses-side-name"}(language->get("licenses", "allLicenses")),
            span{class_ = "licenses-side-pill licenses-side-pill-all"}(
                Nui::observe(impl_->entries).generate([this]() {
                    return std::to_string(impl_->entries.value().size());
                })
            )
        ),
        div{class_ = "licenses-side-list"}(
            Nui::range(impl_->entries),
            [this](long long index, LicenseEntry const& e) -> Nui::ElementRenderer {
                return div{
                    class_ = "licenses-side-row",
                    style = Nui::observe(impl_->filter, impl_->selectedIndex)
                        .generate([this, index, name = e.name]() {
                            const auto& f = impl_->filter.value();
                            const bool matches = f.empty() || toLower(name).find(toLower(f)) != std::string::npos;
                            const bool active = impl_->selectedIndex.value() == index;
                            std::string s;
                            if (!matches)
                                s += "display: none;";
                            if (active)
                                s += "background-color: var(--theme-color-accent, #2a2a2a);";
                            return s;
                        }),
                    onClick = [this, index](Nui::val) {
                        impl_->selectedIndex = index;
                    },
                }(
                    span{class_ = "licenses-side-name"}(e.name),
                    span{
                        class_ = "licenses-side-pill",
                        "data-license"_attr = e.license,
                    }(e.license)
                );
            }
        )
    );
}

Nui::ElementRenderer Licenses::main()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Elements::a;
    using Nui::Elements::pre;

    return div{class_ = "licenses-main"}(
        Nui::observe(impl_->selectedIndex, impl_->entries),
        [this]() -> Nui::ElementRenderer {
            const auto& list = impl_->entries.value();
            const auto idx = impl_->selectedIndex.value();
            if (idx < 0)
            {
                std::string concatenated;
                for (auto const& e : list)
                {
                    concatenated += "==== ";
                    concatenated += e.name;
                    if (!e.version.empty())
                    {
                        concatenated += " (" + e.version + ")";
                    }
                    concatenated += " — " + e.license + " ====\n\n";
                    concatenated += e.text;
                    concatenated += "\n\n";
                }
                return pre{class_ = "licenses-text"}(concatenated);
            }
            if (idx >= static_cast<long long>(list.size()))
                return div{}(language->get("licenses", "noSelection"));
            auto const& e = list[static_cast<std::size_t>(idx)];
            const Nui::ElementRenderer copyrightRow = e.copyright.empty()
                ? Nui::nil()
                : Nui::ElementRenderer{div{class_ = "licenses-detail-copyright"}(e.copyright)};
            const Nui::ElementRenderer homepageRow = e.homepage.empty()
                ? Nui::nil()
                : Nui::ElementRenderer{div{class_ = "licenses-detail-homepage"}(
                    a{
                        href = e.homepage,
                        target = "_blank",
                        rel = "noopener",
                    }(e.homepage)
                  )};
            return div{class_ = "licenses-detail"}(
                div{class_ = "licenses-detail-header"}(
                    span{class_ = "licenses-detail-name"}(e.name),
                    span{class_ = "licenses-detail-version"}(e.version),
                    span{class_ = "licenses-detail-license"}(e.license)
                ),
                copyrightRow,
                homepageRow,
                pre{class_ = "licenses-text"}(e.text.empty() ? language->get("licenses", "noLicenseText") : e.text)
            );
        }
    );
}

Nui::ElementRenderer Licenses::about()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Elements::a;

    const auto detailRow = [](std::string const& label, std::string const& value) -> Nui::ElementRenderer
    {
        if (value.empty() || value == "N/A")
            return Nui::nil();
        return div{class_ = "licenses-about-detail"}(
            span{class_ = "licenses-about-detail-label"}(label),
            span{class_ = "licenses-about-detail-value"}(value)
        );
    };

    return div{class_ = "licenses-about"}(
        div{class_ = "licenses-about-name"}("nui-sftp"),
        div{class_ = "licenses-about-version"}(
            Nui::observe(impl_->events->appVersion).generate([this]() {
                const auto& version = impl_->events->appVersion.value().version;
                return version.empty() ? std::string{"..."} : version;
            })
        ),
        div{class_ = "licenses-about-details"}(
            Nui::observe(impl_->events->appVersion, impl_->events->onLanguageChanged),
            [this, detailRow]() -> Nui::ElementRenderer {
                const auto& version = impl_->events->appVersion.value();
                return Nui::Elements::fragment(
                    detailRow(language->get("licenses", "gitTag"), version.gitTag),
                    detailRow(language->get("licenses", "gitBranch"), version.gitBranch)
                );
            }
        ),
        a{
            class_ = "licenses-about-link",
            href = "https://github.com/5cript/nui-sftp",
            target = "_blank",
            rel = "noopener",
        }("github.com/5cript/nui-sftp")
    );
}

Nui::ElementRenderer Licenses::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    Nui::listen(impl_->events->licensesOpen, [this](bool isOpen) {
        if (!isOpen)
            return;
        loadIfNeeded();
        impl_->selectTab(impl_->licensesTabId);
    });

    impl_->languageListener = language->listenToLanguageChange([this](std::string const&) {
        const auto retitle = [this](int tabId, char const* key) {
            impl_->tabs.modifyTabById(tabId, [key](ScriptNuiComponents::Tabs::Tab* tab) {
                if (tab)
                    tab->title = language->get("licenses", key);
            });
        };
        retitle(impl_->licensesTabId, "licensesTab");
        retitle(impl_->aboutTabId, "aboutTab");
    });


    return div{
        class_ = "licenses-page-background-blocker",
        style = Nui::observe(impl_->events->licensesOpen).generate([](bool isOpen) -> std::string {
            return isOpen ? "display: flex;" : "display: none;";
        }),
        onKeyDown = [this](Nui::val event) {
            const auto key = event["key"].as<std::string>();
            if (key == "Escape")
                impl_->events->licensesOpen = false;
        },
        tabIndex = "0",
    }(
        div{class_ = "licenses-page"}(
            header(),
            div{class_ = "licenses-page-tab-strip"}(
                impl_->tabs({"data-class"_attr = "licenses-page-tabs"})
            ),
            // The about page covers the licenses instead of toggling them, since any style change
            // on the licenses relays out the large license text, which takes a few hundred ms.
            div{class_ = "licenses-page-content"}(
                sidebar(),
                main()
            ),
            div{
                class_ = "licenses-about-page",
                style = Nui::observe(impl_->activeTabId).generate([this]() -> std::string {
                    return impl_->activeTabId.value() == impl_->aboutTabId ? std::string{} : std::string{"visibility: hidden;"};
                }),
            }(
                about()
            )
        )
    );
}
