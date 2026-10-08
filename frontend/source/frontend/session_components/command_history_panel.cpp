#include <frontend/session_components/command_history_panel.hpp>
#include <frontend/session_components/command_panel_helpers.hpp>

#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/pill_list.hpp>
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
#include <ui5-sap-icons/icons/clear-all.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/svg_elements.hpp>
#include <nui/frontend/svg_attributes.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/event_system/observed_value_combinator.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <variant>
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

        bool operator==(MergedHistoryEntry const&) const = default;
    };

    /// Header of the pinned strip or a day group; collapsing it leaves out the group's rows.
    struct GroupHeader
    {
        std::string label{};
        std::size_t count{0};
        bool expanded{true};

        bool operator==(GroupHeader const&) const = default;
    };

    /// A command row with everything it displays, so an unchanged row compares equal.
    struct CommandRow
    {
        MergedHistoryEntry entry{};
        std::string timeLabel{};
        std::string loweredQuery{};
        bool selected{false};

        bool operator==(CommandRow const&) const = default;
    };

    /// One line of the list. The list is updated by key, so only lines that changed are redrawn.
    struct HistoryRow
    {
        std::string key{};
        std::variant<GroupHeader, CommandRow> content{};

        bool operator==(HistoryRow const&) const = default;
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

    /// Hosts seen in any load of this panel's lifetime.
    std::set<std::string> knownHosts{};
    std::shared_ptr<Nui::Observed<std::vector<ScriptNuiComponents::PillOptions>>> hostPills{
        std::make_shared<Nui::Observed<std::vector<ScriptNuiComponents::PillOptions>>>()
    };
    /// Expansion state of the day groups, keyed by bucket label.
    std::map<std::string, bool> expandedGroups{};
    /// What the list shows, derived from the history and the filters by refreshRows.
    Nui::Observed<std::vector<HistoryRow>> rows{};
    Nui::ListenRemover<Nui::Observed<std::vector<CommandStore::HistoryEntry>>> historyListener{};
    Nui::ListenRemover<Nui::Observed<std::string>> searchListener{};
    Nui::ListenRemover<Nui::Observed<bool>> favoritesListener{};
    Nui::ListenRemover<Nui::Observed<std::set<std::int64_t>>> selectionListener{};

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

    /// The client's list is shared by every session, so it stays unfiltered; host filter and sort
    /// apply to this panel alone, in visibleEntries.
    void reload()
    {
        client->reloadHistory();
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
                        refreshRows();
                    },
            });
        }
        *hostPills = std::move(pills);
    }

    /// The loaded entries passing the host, search and favorites filters, identical commands of
    /// different hosts merged into one row, sorted by the active sort.
    std::vector<MergedHistoryEntry> visibleEntries() const
    {
        const auto loweredQuery = lowercased(searchQuery.value());

        std::vector<MergedHistoryEntry> visible{};
        std::map<std::string, std::size_t> indexByCommand{};
        for (auto const& entry : client->history().value())
        {
            if (hostFilter && entry.host != *hostFilter)
                continue;
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

    void clearAll()
    {
        const auto count = client->history().value().size();
        if (count == 0)
            return;

        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = std::string{language->get("commandHistoryPanel", "confirmClearHeader")},
            .text = fmt::format(
                fmt::runtime(std::string{language->get("commandHistoryPanel", "confirmClearText")}), count
            ),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .onClose =
                [this](std::optional<ConfirmDialog::Button> button) {
                    if (!button || *button != ConfirmDialog::Button::Yes)
                        return;
                    client->clearHistory();
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

    /// The pinned strip and the day groups, flattened into header and command rows.
    std::vector<HistoryRow> computeRows() const
    {
        const auto loweredQuery = lowercased(searchQuery.value());
        const auto visible = visibleEntries();
        const auto nowEpoch = static_cast<std::int64_t>(std::time(nullptr));

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

        std::vector<HistoryRow> result{};
        result.reserve(visible.size() + groups.size() + 1);
        const auto appendGroup = [&](std::string const& label, std::vector<MergedHistoryEntry const*> const& entries) {
            const auto found = expandedGroups.find(label);
            const bool expanded = found == expandedGroups.end() || found->second;
            result.push_back(HistoryRow{
                .key = fmt::format("group:{}", label),
                .content = GroupHeader{.label = label, .count = entries.size(), .expanded = expanded},
            });
            if (!expanded)
                return;
            for (auto const* entry : entries)
            {
                result.push_back(HistoryRow{
                    .key = fmt::format("command:{}", entry->command),
                    .content =
                        CommandRow{
                            .entry = *entry,
                            .timeLabel = relativeTime(entry->lastRun, nowEpoch),
                            .loweredQuery = loweredQuery,
                            .selected = isSelected(entry->ids),
                        },
                });
            }
        };

        if (!pinned.empty())
            appendGroup(std::string{language->get("commandHistoryPanel", "pinned")}, pinned);
        for (auto const& [bucket, entries] : groups)
            appendGroup(dayBucketLabel(bucket), entries);
        return result;
    }

    void refreshRows()
    {
        CommandPanels::updateKeyed(rows, computeRows(), [](HistoryRow const& row) -> std::string const& {
            return row.key;
        });
    }

    void toggleGroup(std::string const& label)
    {
        const auto found = expandedGroups.find(label);
        expandedGroups[label] = found != expandedGroups.end() && !found->second;
        refreshRows();
    }

    Nui::ElementRenderer renderRow(HistoryRow const& row);
    Nui::ElementRenderer renderGroupHeader(GroupHeader const& header);
    Nui::ElementRenderer renderCommand(CommandRow const& row);
    Nui::ElementRenderer renderBulkBar();
};

Nui::ElementRenderer CommandHistoryPanel::Implementation::renderRow(HistoryRow const& row)
{
    if (auto const* header = std::get_if<GroupHeader>(&row.content))
        return renderGroupHeader(*header);
    return renderCommand(std::get<CommandRow>(row.content));
}

Nui::ElementRenderer CommandHistoryPanel::Implementation::renderGroupHeader(GroupHeader const& header)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    namespace svgElements = Nui::Elements::Svg;
    namespace svgAttributes = Nui::Attributes::Svg;

    // Looks like the components library's collapsible section, whose body cannot hold rows of a
    // flat list.
    // clang-format off
    return div{
        class_ = header.expanded ? "cmdh-group-header cmdh-group-header-expanded" : "cmdh-group-header",
        onClick = [this, label = header.label](Nui::val) {
            toggleGroup(label);
        },
    }(
        svgElements::svg{
            class_ = "cmdh-group-chevron",
            svgAttributes::viewBox = "0 0 16 16",
        }(svgElements::path{
            svgAttributes::d = "M6 3 L11 8 L6 13",
            "fill"_attr = "none",
            "stroke"_attr = "currentColor",
            "stroke-width"_attr = "2",
            "stroke-linecap"_attr = "round",
            "stroke-linejoin"_attr = "round",
        }()),
        span{class_ = "cmdh-group-title"}(header.label),
        span{class_ = "cmdh-group-badge"}(fmt::format("{}", header.count))
    );
    // clang-format on
}

Nui::ElementRenderer CommandHistoryPanel::Implementation::renderCommand(CommandRow const& row)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    auto const& entry = row.entry;
    const bool selected = row.selected;

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
            CommandPanels::highlightedText(entry.command, row.loweredQuery, "cmdh-command"),
            div{class_ = "cmdh-row-meta"}(
                div{class_ = "cmdh-row-hosts"}(
                    Nui::range(std::move(hostElements)),
                    [](long long, auto const& hostElement) -> Nui::ElementRenderer {
                        return hostElement;
                    }
                ),
                span{class_ = "cmdh-time"}(row.timeLabel),
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
        auto* const implementation = impl_.get();
        const auto refresh = [implementation](auto const&) {
            implementation->refreshRows();
            Nui::globalEventContext.executeActiveEventsImmediately();
        };
        impl_->historyListener = Nui::smartListen(impl_->client->history(), [implementation](auto const&) {
            implementation->rebuildHostPills();
            implementation->refreshRows();
            Nui::globalEventContext.executeActiveEventsImmediately();
        });
        impl_->searchListener = Nui::smartListen(impl_->searchQuery, refresh);
        impl_->favoritesListener = Nui::smartListen(impl_->favoritesOnly, refresh);
        impl_->selectionListener = Nui::smartListen(impl_->selectedIds, refresh);
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
    impl_->refreshRows();

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
                                impl_->refreshRows();
                            },
                    }
                ),
                button{
                    class_ = "cmdh-action cmdh-action-danger",
                    title = language->get("commandHistoryPanel", "clearTooltip"),
                    onClick = [this](Nui::val) {
                        impl_->clearAll();
                    },
                }(Ui5Icons::clear_all())
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
            div{class_ = "cmdh-empty-host"}(
                Nui::observe(impl_->rows, impl_->client->history()).generate([this]() -> Nui::ElementRenderer {
                    if (!impl_->rows.value().empty())
                        return Nui::nil();
                    return div{class_ = "cmdh-empty"}(language->get(
                        "commandHistoryPanel", impl_->client->history().value().empty() ? "emptyText" : "noMatchesText"
                    ));
                })
            ),
            // Bound to the observed rows: a change redraws only the rows it touched.
            div{class_ = "cmdh-rows"}(
                Nui::range(impl_->rows),
                [this](long long, HistoryRow const& row) -> Nui::ElementRenderer {
                    return impl_->renderRow(row);
                }
            )
        )
    );
    // clang-format on
}
