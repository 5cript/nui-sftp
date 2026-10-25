#include <frontend/session_components/file_tracking/detail_tabs.hpp>
#include <frontend/session_components/file_tracking/conflicts_tab.hpp>
#include <frontend/session_components/file_tracking/failures_tab.hpp>
#include <frontend/session_components/file_tracking/options_tab.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/tab_strip.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/event_system/range.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace FileTracking
{
    namespace
    {
        ScriptNuiComponents::TabStripTab tabOf(DetailTabEntry const& entry, WatchItem const& item)
        {
            auto count = entry.count ? entry.count(item) : nullptr;
            std::optional<ScriptNuiComponents::TabStripBadge> badge{};
            if (count)
                badge =
                    ScriptNuiComponents::TabStripBadge{.count = std::move(count), .styleVariant = entry.badgeVariant};
            return {
                .key = Utility::enumToString(entry.tab),
                .title = entry.title,
                .badge = std::move(badge),
            };
        }

        /**
         * @brief The panel of @p entry, mounted while its tab is not chosen so its list keeps its scroll position.
         */
        Nui::ElementRenderer panelOf(
            DetailTabEntry const& entry,
            TrackingStore& store,
            TrackingActions const& actions,
            std::shared_ptr<WatchItem> const& item
        )
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            const auto key = Utility::enumToString(entry.tab);

            // clang-format off
            return div{
                class_ = "file-tracking-tab-panel",
                role = "tabpanel",
                "aria-label"_attr = entry.title,
                "data-ft-tab-panel"_attr = key,
                hidden = Nui::observe(store.detailTabKey()).generate(
                    [key](std::string const& chosen)
                    {
                        return chosen != key;
                    }
                ),
            }(
                entry.render ? entry.render(store, actions, item) : Nui::nil()
            );
            // clang-format on
        }
    }

    std::vector<DetailTabEntry> detailTabRegistry()
    {
        return {
            DetailTabEntry{
                .tab = DetailTab::Conflicts,
                .title = language->get("fileTracking", "tabConflicts"),
                .count =
                    [](WatchItem const& item)
                {
                    return item.conflictCount;
                },
                .badgeVariant = ScriptNuiComponents::StyleVariant::Warning,
                .render = &conflictsTab,
            },
            DetailTabEntry{
                .tab = DetailTab::Failed,
                .title = language->get("fileTracking", "tabFailed"),
                .count =
                    [](WatchItem const& item)
                {
                    return item.failureCount;
                },
                .badgeVariant = ScriptNuiComponents::StyleVariant::Danger,
                .render = &failuresTab,
            },
            DetailTabEntry{
                .tab = DetailTab::Options,
                .title = language->get("fileTracking", "tabOptions"),
                .render = &optionsTab,
            },
        };
    }

    Nui::ElementRenderer
    detailTabs(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        const auto registry = detailTabRegistry();

        std::vector<ScriptNuiComponents::TabStripTab> tabs{};
        std::vector<Nui::ElementRenderer> panels{};
        std::vector<DetailTab> order{};
        tabs.reserve(registry.size());
        panels.reserve(registry.size());
        order.reserve(registry.size());
        for (auto const& entry : registry)
        {
            tabs.push_back(tabOf(entry, *item));
            panels.push_back(panelOf(entry, store, actions, item));
            order.push_back(entry.tab);
        }

        // clang-format off
        return div{class_ = "file-tracking-detail-tabs"}(
            ScriptNuiComponents::tabStrip({
                .tabs = std::move(tabs),
                .selected = store.detailTabKey(),
                .onSelect = [&actions, order = std::move(order)](std::string const& key)
                {
                    const auto chosen = std::ranges::find_if(order, [&key](DetailTab tab)
                    {
                        return Utility::enumToString(tab) == key;
                    });
                    if (chosen != order.end() && actions.chooseTab)
                        actions.chooseTab(*chosen);
                },
                .label = language->get("fileTracking", "detailTabsLabel"),
            }),
            div{class_ = "file-tracking-tab-panels"}(
                Nui::range(std::move(panels)),
                [](long long, Nui::ElementRenderer const& panel) -> Nui::ElementRenderer
                {
                    return panel;
                }
            )
        );
        // clang-format on
    }
}
