#include <frontend/session_components/command_history_panel.hpp>
#include <frontend/session_components/command_panel_helpers.hpp>

#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/pill_list.hpp>
#include <script-nui-components/collapsible_section.hpp>
#include <script-nui-components/checkbox.hpp>
#include <script-nui-components/select.hpp>
#include <ui5-sap-icons/icons/search.hpp>
#include <ui5-sap-icons/icons/favorite.hpp>
#include <ui5-sap-icons/icons/unfavorite.hpp>
#include <ui5-sap-icons/icons/pushpin-on.hpp>
#include <ui5-sap-icons/icons/pushpin-off.hpp>
#include <ui5-sap-icons/icons/media-play.hpp>
#include <ui5-sap-icons/icons/edit.hpp>
#include <ui5-sap-icons/icons/copy.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/decline.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/event_system/observed_value_combinator.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace std::string_literals;

namespace
{
    using CommandPanels::lowercased;
    using CommandPanels::matchesSearch;
    using CommandPanels::relativeTime;

    constexpr std::int64_t secondsPerDay = 86'400;
    constexpr int earlierBucketThresholdDays = 14;

    /// Days since the epoch on the local calendar, so groups change at local midnight.
    std::int64_t localDay(std::int64_t epochSeconds)
    {
        const auto time = static_cast<std::time_t>(epochSeconds);
        std::tm local{};
        localtime_r(&time, &local);
        return (epochSeconds + local.tm_gmtoff) / secondsPerDay;
    }

    /// Bucket key by age in days; doubles as the ordering key of the day groups.
    int dayBucket(std::int64_t epochSeconds, std::int64_t nowEpoch)
    {
        const auto days = static_cast<int>(localDay(nowEpoch) - localDay(epochSeconds));
        return std::clamp(days, 0, earlierBucketThresholdDays + 1);
    }

    std::string dayBucketLabel(int bucket)
    {
        if (bucket <= 0)
            return std::string{language->get("commandHistoryPanel", "today")};
        if (bucket == 1)
            return std::string{language->get("commandHistoryPanel", "yesterday")};
        if (bucket <= earlierBucketThresholdDays)
            return fmt::format(
                fmt::runtime(std::string{language->get("commandHistoryPanel", "daysAgoGroup")}), bucket
            );
        return std::string{language->get("commandHistoryPanel", "earlier")};
    }

    /// Stable pastel per host so the dot color survives reloads without storing anything.
    std::string hostDotStyle(std::string const& host)
    {
        const auto hash = std::hash<std::string>{}(host);
        return fmt::format("background-color: hsl({}, 60%, 55%);", hash % 360);
    }

    void copyToClipboard(std::string const& textToCopy)
    {
        Nui::val::global("navigator")["clipboard"].call<Nui::val>("writeText", textToCopy);
    }

    /// One panel row: identical commands from different hosts fold into a single entry.
    struct MergedHistoryEntry
    {
        std::string command{};
        std::vector<std::int64_t> ids{};
        std::vector<std::string> hosts{};
        std::int64_t runs{0};
        std::int64_t lastRun{0};
        bool pinned{false};
        bool favorite{false};
    };
}

struct CommandHistoryPanel::Implementation
{
    CommandStoreClient* client;
    FrontendEvents* events;
    ConfirmDialog* confirmDialog;
    std::function<void(std::string const&, bool)> runInTerminal;
    Nui::Observed<bool>* connectionLost;

    Nui::Observed<std::string> searchQuery{};
    Nui::Observed<bool> favoritesOnly{false};
    Nui::Observed<std::set<std::int64_t>> selectedIds{};
    std::optional<std::string> hostFilter{};
    CommandStore::SortOrder sort{CommandStore::SortOrder::Recent};
    std::vector<std::string> sortLabels{
        std::string{language->get("commandHistoryPanel", "sortRecent")},
        std::string{language->get("commandHistoryPanel", "sortMostRun")},
        std::string{language->get("commandHistoryPanel", "sortName")},
    };
    Nui::Observed<std::string> sortLabel{sortLabels.front()};

    /// Hosts seen in any load of this panel's lifetime; the filtered reload would otherwise
    /// collapse the pill row to the filtered host alone.
    std::set<std::string> knownHosts{};
    std::shared_ptr<Nui::Observed<std::vector<ScriptNuiComponents::PillOptions>>> hostPills{
        std::make_shared<Nui::Observed<std::vector<ScriptNuiComponents::PillOptions>>>()
    };
    /// Expansion state of the day groups, keyed by bucket label; survives list regeneration.
    std::map<std::string, bool> expandedGroups{};
    Nui::ListenRemover<Nui::Observed<std::vector<CommandStore::HistoryEntry>>> historyListener{};

    Implementation(
        CommandStoreClient* client,
        FrontendEvents* events,
        ConfirmDialog* confirmDialog,
        std::function<void(std::string const&, bool)> runInTerminal,
        Nui::Observed<bool>* connectionLost
    )
        : client{client}
        , events{events}
        , confirmDialog{confirmDialog}
        , runInTerminal{std::move(runInTerminal)}
        , connectionLost{connectionLost}
    {}

    void reload()
    {
        client->reloadHistory(CommandStore::HistoryQuery{
            .hostFilter = hostFilter,
            .sort = sort,
            .limit = std::nullopt,
        });
    }

    void rebuildHostPills()
    {
        for (auto const& entry : client->history().value())
            knownHosts.insert(entry.host);

        std::vector<ScriptNuiComponents::PillOptions> pills{};
        pills.reserve(knownHosts.size());
        for (auto const& host : knownHosts)
        {
            pills.push_back(ScriptNuiComponents::PillOptions{
                .text = host,
                .selected = hostFilter == host,
                .onClick =
                    [this, host]() {
                        hostFilter = hostFilter == host ? std::nullopt : std::optional<std::string>{host};
                        rebuildHostPills();
                        reload();
                    },
            });
        }
        *hostPills = std::move(pills);
    }

    /// The loaded entries passing the search and favorites filters, identical commands of
    /// different hosts merged into one row, sorted by the active sort.
    std::vector<MergedHistoryEntry> visibleEntries() const
    {
        const auto loweredQuery = lowercased(searchQuery.value());

        std::vector<MergedHistoryEntry> visible{};
        std::map<std::string, std::size_t> indexByCommand{};
        for (auto const& entry : client->history().value())
        {
            if (favoritesOnly.value() && !entry.favorite)
                continue;
            if (!matchesSearch(loweredQuery, entry.command))
                continue;

            const auto found = indexByCommand.find(entry.command);
            if (found == indexByCommand.end())
            {
                indexByCommand.emplace(entry.command, visible.size());
                visible.push_back(MergedHistoryEntry{
                    .command = entry.command,
                    .ids = {entry.id},
                    .hosts = {entry.host},
                    .runs = entry.runs,
                    .lastRun = entry.lastRun,
                    .pinned = entry.pinned,
                    .favorite = entry.favorite,
                });
                continue;
            }

            auto& merged = visible[found->second];
            merged.ids.push_back(entry.id);
            if (std::ranges::find(merged.hosts, entry.host) == merged.hosts.end())
                merged.hosts.push_back(entry.host);
            merged.runs += entry.runs;
            merged.lastRun = std::max(merged.lastRun, entry.lastRun);
            merged.pinned = merged.pinned || entry.pinned;
            merged.favorite = merged.favorite || entry.favorite;
        }

        // Merging may reorder relative to the database sort, so sort the merged rows again.
        switch (sort)
        {
            case CommandStore::SortOrder::MostRun:
                std::ranges::stable_sort(visible, [](MergedHistoryEntry const& a, MergedHistoryEntry const& b) {
                    return a.runs > b.runs;
                });
                break;
            case CommandStore::SortOrder::Name:
                std::ranges::stable_sort(visible, [](MergedHistoryEntry const& a, MergedHistoryEntry const& b) {
                    return lowercased(a.command) < lowercased(b.command);
                });
                break;
            case CommandStore::SortOrder::Recent:
                std::ranges::stable_sort(visible, [](MergedHistoryEntry const& a, MergedHistoryEntry const& b) {
                    return a.lastRun > b.lastRun;
                });
                break;
        }
        return visible;
    }

    bool isSelected(std::vector<std::int64_t> const& ids) const
    {
        return std::ranges::any_of(ids, [this](std::int64_t id) {
            return selectedIds.value().contains(id);
        });
    }

    void toggleSelection(std::vector<std::int64_t> const& ids)
    {
        auto& selected = selectedIds.value();
        if (isSelected(ids))
        {
            for (const auto id : ids)
                selected.erase(id);
        }
        else
            selected.insert(ids.begin(), ids.end());
        selectedIds.modify();
    }

    void deleteSelected()
    {
        const std::vector<std::int64_t> ids{selectedIds.value().begin(), selectedIds.value().end()};
        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = std::string{language->get("commandHistoryPanel", "confirmDeleteHeader")},
            .text = fmt::format(
                fmt::runtime(std::string{language->get("commandHistoryPanel", "confirmDeleteText")}), ids.size()
            ),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .onClose =
                [this, ids](std::optional<ConfirmDialog::Button> button) {
                    if (!button || *button != ConfirmDialog::Button::Yes)
                        return;
                    client->deleteHistory(ids);
                    selectedIds.value().clear();
                    selectedIds.modify();
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
        });
    }

    void favoriteSelected()
    {
        for (const auto id : selectedIds.value())
            client->setHistoryFlags(id, std::nullopt, true);
        selectedIds.value().clear();
        selectedIds.modify();
    }

    Nui::ElementRenderer renderRow(MergedHistoryEntry const& entry, std::string const& loweredQuery);
    Nui::ElementRenderer renderGroups();
    Nui::ElementRenderer renderBulkBar();
};

Nui::ElementRenderer
CommandHistoryPanel::Implementation::renderRow(MergedHistoryEntry const& entry, std::string const& loweredQuery)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    const auto nowEpoch = static_cast<std::int64_t>(std::time(nullptr));
    const bool selected = isSelected(entry.ids);

    std::vector<Nui::ElementRenderer> hostElements{};
    hostElements.reserve(entry.hosts.size() * 2);
    for (auto const& host : entry.hosts)
    {
        hostElements.push_back(span{class_ = "cmdh-host-dot", style = hostDotStyle(host)}());
        hostElements.push_back(span{class_ = "cmdh-host"}(host));
    }

    // clang-format off
    return div{
        class_ = selected ? "cmdh-row cmdh-row-selected" : "cmdh-row",
    }(
        // No class_ in component attributes: a second class attribute would override the
        // component's own class and strip its styling.
        ScriptNuiComponents::checkbox({
            .isChecked = selected,
            .attributes = {
                title = language->get("commandHistoryPanel", "selectTooltip"),
            },
            .onChange = [this, ids = entry.ids](bool, Nui::WebApi::MouseEvent const&) {
                toggleSelection(ids);
            },
        }),
        div{class_ = "cmdh-row-main"}(
            CommandPanels::highlightedText(entry.command, loweredQuery, "cmdh-command"),
            div{class_ = "cmdh-row-meta"}(
                div{class_ = "cmdh-row-hosts"}(
                    Nui::range(std::move(hostElements)),
                    [](long long, auto const& hostElement) -> Nui::ElementRenderer {
                        return hostElement;
                    }
                ),
                span{class_ = "cmdh-time"}(relativeTime(entry.lastRun, nowEpoch)),
                entry.runs > 1
                    ? Nui::ElementRenderer{span{class_ = "cmdh-runs"}(fmt::format("×{}", entry.runs))}
                    : Nui::nil()
            )
        ),
        div{class_ = "cmdh-row-actions"}(
            button{
                class_ = "cmdh-action",
                CommandPanels::connectionTooltip(
                    *connectionLost, std::string{language->get("commandHistoryPanel", "runTooltip")}),
                CommandPanels::disabledWhileDisconnected(*connectionLost),
                onClick = [this, command = entry.command](Nui::val) {
                    runInTerminal(command, true);
                },
            }(Ui5Icons::media_play()),
            button{
                class_ = "cmdh-action",
                CommandPanels::connectionTooltip(
                    *connectionLost, std::string{language->get("commandHistoryPanel", "editAndRunTooltip")}),
                CommandPanels::disabledWhileDisconnected(*connectionLost),
                onClick = [this, command = entry.command](Nui::val) {
                    runInTerminal(command, false);
                },
            }(Ui5Icons::edit()),
            button{
                class_ = "cmdh-action",
                title = language->get("commandHistoryPanel", "copyTooltip"),
                onClick = [command = entry.command](Nui::val) {
                    copyToClipboard(command);
                },
            }(Ui5Icons::copy()),
            button{
                class_ = entry.pinned ? "cmdh-action cmdh-action-active" : "cmdh-action",
                title = language->get("commandHistoryPanel", "pinTooltip"),
                onClick = [this, ids = entry.ids, pinned = entry.pinned](Nui::val) {
                    for (const auto id : ids)
                        client->setHistoryFlags(id, !pinned, std::nullopt);
                },
            }(entry.pinned ? Ui5Icons::pushpin_on() : Ui5Icons::pushpin_off()),
            button{
                class_ = "cmdh-action cmdh-action-danger",
                title = language->get("commandHistoryPanel", "deleteTooltip"),
                onClick = [this, ids = entry.ids](Nui::val) {
                    client->deleteHistory(ids);
                },
            }(Ui5Icons::delete_()),
            // Last, so the always visible star of favorites lines up on the row's edge.
            button{
                class_ = entry.favorite ? "cmdh-action cmdh-action-active cmdh-action-favorited" : "cmdh-action",
                title = language->get("commandHistoryPanel", "favoriteTooltip"),
                onClick = [this, ids = entry.ids, favorite = entry.favorite](Nui::val) {
                    for (const auto id : ids)
                        client->setHistoryFlags(id, std::nullopt, !favorite);
                },
            }(entry.favorite ? Ui5Icons::favorite() : Ui5Icons::unfavorite())
        )
    );
    // clang-format on
}

Nui::ElementRenderer CommandHistoryPanel::Implementation::renderGroups()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    const auto loweredQuery = lowercased(searchQuery.value());
    const auto visible = visibleEntries();
    const auto nowEpoch = static_cast<std::int64_t>(std::time(nullptr));

    if (visible.empty())
    {
        return div{class_ = "cmdh-empty"}(
            language->get(
                "commandHistoryPanel", client->history().value().empty() ? "emptyText" : "noMatchesText"
            )
        );
    }

    std::vector<MergedHistoryEntry const*> pinned{};
    // Bucket index to entries, ordered oldest bucket last; entries keep their sort order.
    std::map<int, std::vector<MergedHistoryEntry const*>> groups{};
    for (auto const& entry : visible)
    {
        if (entry.pinned)
            pinned.push_back(&entry);
        else
            groups[dayBucket(entry.lastRun, nowEpoch)].push_back(&entry);
    }

    const auto renderSection =
        [this, &loweredQuery](std::string const& label, std::vector<MergedHistoryEntry const*> const& entries)
    {
        std::vector<Nui::ElementRenderer> rows{};
        rows.reserve(entries.size());
        for (auto const* entry : entries)
            rows.push_back(renderRow(*entry, loweredQuery));

        const auto expanded = expandedGroups.find(label);
        return ScriptNuiComponents::collapsibleSection(
            {
                .title = label,
                .badge = fmt::format("{}", entries.size()),
                .initiallyExpanded = expanded == expandedGroups.end() ? true : expanded->second,
                .onToggle =
                    [this, label](bool isExpanded) {
                        expandedGroups[label] = isExpanded;
                    },
            },
            div{class_ = "cmdh-group-rows"}(
                Nui::range(std::move(rows)),
                [](long long, auto const& row) -> Nui::ElementRenderer {
                    return row;
                }
            )
        );
    };

    std::vector<Nui::ElementRenderer> sections{};
    sections.reserve(groups.size() + 1);
    if (!pinned.empty())
        sections.push_back(renderSection(std::string{language->get("commandHistoryPanel", "pinned")}, pinned));
    for (auto const& [bucket, entries] : groups)
        sections.push_back(renderSection(dayBucketLabel(bucket), entries));

    return div{class_ = "cmdh-groups"}(
        Nui::range(std::move(sections)),
        [](long long, auto const& section) -> Nui::ElementRenderer {
            return section;
        }
    );
}

Nui::ElementRenderer CommandHistoryPanel::Implementation::renderBulkBar()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    const auto selectedCount = selectedIds.value().size();
    if (selectedCount == 0)
        return Nui::nil();

    // clang-format off
    return div{class_ = "cmdh-bulk-bar"}(
        span{class_ = "cmdh-bulk-count"}(
            fmt::format(
                fmt::runtime(std::string{language->get("commandHistoryPanel", "selectedCount")}), selectedCount
            )
        ),
        button{
            class_ = "cmdh-action",
            title = language->get("commandHistoryPanel", "bulkFavoriteTooltip"),
            onClick = [this](Nui::val) {
                favoriteSelected();
            },
        }(Ui5Icons::favorite()),
        button{
            class_ = "cmdh-action cmdh-action-danger",
            title = language->get("commandHistoryPanel", "bulkDeleteTooltip"),
            onClick = [this](Nui::val) {
                deleteSelected();
            },
        }(Ui5Icons::delete_()),
        button{
            class_ = "cmdh-action",
            title = language->get("commandHistoryPanel", "clearSelectionTooltip"),
            onClick = [this](Nui::val) {
                selectedIds.value().clear();
                selectedIds.modify();
            },
        }(Ui5Icons::decline())
    );
    // clang-format on
}

CommandHistoryPanel::CommandHistoryPanel(
    CommandStoreClient* commandStoreClient,
    FrontendEvents* events,
    ConfirmDialog* confirmDialog,
    std::function<void(std::string const&, bool)> runInTerminal,
    Nui::Observed<bool>* connectionLost
)
    : impl_{std::make_unique<Implementation>(
          commandStoreClient, events, confirmDialog, std::move(runInTerminal), connectionLost
      )}
{
    // A null client means the backend store failed to open; the panel then only shows a notice.
    if (impl_->client)
    {
        impl_->historyListener = Nui::smartListen(
            impl_->client->history(),
            [implementation = impl_.get()](auto const&) {
                implementation->rebuildHostPills();
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        );
    }
}
CommandHistoryPanel::~CommandHistoryPanel() = default;
CommandHistoryPanel::CommandHistoryPanel(CommandHistoryPanel&&) = default;
CommandHistoryPanel& CommandHistoryPanel::operator=(CommandHistoryPanel&&) = default;

Nui::ElementRenderer CommandHistoryPanel::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Attributes::title;

    if (!impl_->client)
    {
        return div{class_ = "command-history-panel"}(
            div{class_ = "cmdh-empty"}(language->get("commandHistoryPanel", "storeUnavailable"))
        );
    }

    impl_->reload();

    // clang-format off
    return div{class_ = "command-history-panel"}(
        div{class_ = "cmdh-header"}(
            div{class_ = "cmdh-search-row"}(
                div{class_ = "cmdh-search-box"}(
                    Ui5Icons::search(),
                    input{
                        type = "text",
                        class_ = "cmdh-search",
                        placeHolder = language->get("commandHistoryPanel", "searchPlaceholder"),
                        onInput = [this](Nui::val event) {
                            impl_->searchQuery = event["target"]["value"].as<std::string>();
                        },
                    }()
                ),
                button{
                    class_ = Nui::observe(impl_->favoritesOnly).generate([this]() {
                        return impl_->favoritesOnly.value()
                            ? "cmdh-action cmdh-action-active"s
                            : "cmdh-action"s;
                    }),
                    title = language->get("commandHistoryPanel", "favoritesOnlyTooltip"),
                    onClick = [this](Nui::val) {
                        impl_->favoritesOnly = !impl_->favoritesOnly.value();
                    },
                }(Ui5Icons::favorite()),
                ScriptNuiComponents::select(
                    ScriptNuiComponents::SelectOptions<decltype(impl_->sortLabel), std::vector<std::string>>{
                        .activeOption = impl_->sortLabel,
                        .options = impl_->sortLabels,
                        .attributes =
                            {
                                title = language->get("commandHistoryPanel", "sortTooltip"),
                            },
                        .onChange =
                            [this](std::string const& newValue, Nui::WebApi::MouseEvent const&) {
                                if (newValue == impl_->sortLabels[1])
                                    impl_->sort = CommandStore::SortOrder::MostRun;
                                else if (newValue == impl_->sortLabels[2])
                                    impl_->sort = CommandStore::SortOrder::Name;
                                else
                                    impl_->sort = CommandStore::SortOrder::Recent;
                                impl_->reload();
                            },
                    }
                )
            ),
            ScriptNuiComponents::pillList({
                .pills = impl_->hostPills,
            })
        ),
        div{class_ = "cmdh-bulk-host"}(
            Nui::observe(impl_->selectedIds).generate([this]() -> Nui::ElementRenderer {
                return impl_->renderBulkBar();
            })
        ),
        div{class_ = "cmdh-list"}(
            Nui::observe(impl_->client->history(), impl_->searchQuery, impl_->favoritesOnly, impl_->selectedIds)
                .generate([this]() -> Nui::ElementRenderer {
                    return impl_->renderGroups();
                })
        )
    );
    // clang-format on
}
