#include <frontend/session_components/file_tracking/outliner.hpp>
#include <frontend/session_components/file_tracking/orphan_row.hpp>
#include <frontend/session_components/file_tracking/watch_row.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/badge.hpp>
#include <script-nui-components/button.hpp>
#include <script-nui-components/collapsible_section.hpp>
#include <script-nui-components/style_variant.hpp>
#include <script-nui-components/text_input.hpp>

#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/search.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FileTracking
{
    namespace
    {
        std::string titleOf(OutlinerGroup group)
        {
            switch (group)
            {
                case OutlinerGroup::NeedsYou:
                    return language->get("fileTracking", "groupNeedsYou");
                case OutlinerGroup::Syncing:
                    return language->get("fileTracking", "groupSyncing");
                case OutlinerGroup::Watching:
                    return language->get("fileTracking", "statusWatching");
                case OutlinerGroup::Stopped:
                    return language->get("fileTracking", "groupStopped");
                case OutlinerGroup::Orphaned:
                    return language->get("fileTracking", "groupOrphaned");
            }
            return {};
        }

        std::optional<SelectionMove> selectionMoveOf(Nui::WebApi::KeyboardEvent const& event)
        {
            if (event.altKey() || event.ctrlKey() || event.metaKey() || event.shiftKey())
                return std::nullopt;
            const auto key = event.key();
            if (key == "ArrowUp")
                return SelectionMove::Previous;
            if (key == "ArrowDown")
                return SelectionMove::Next;
            if (key == "Home")
                return SelectionMove::First;
            if (key == "End")
                return SelectionMove::Last;
            return std::nullopt;
        }

        void moveSelectionByKey(TrackingActions const& actions, Nui::WebApi::KeyboardEvent const& event)
        {
            const auto move = selectionMoveOf(event);
            if (!move)
                return;
            event.preventDefault();
            actions.moveSelection(*move);
            Nui::globalEventContext.executeActiveEventsImmediately();
            const auto selector = std::string{".script-nui-list-row[data-selected]"};
            const auto selected = event.currentTarget().call<Nui::val>("querySelector", selector);
            if (!selected.isNull())
                selected.call<void>("focus");
        }

        Nui::ElementRenderer
        groupTrash(std::string const& action, std::string const& tooltip, std::function<void()> const& onRemove)
        {
            using namespace Nui::Attributes;
            namespace Snc = ScriptNuiComponents;

            // clang-format off
            return Snc::button({
                .icon = Ui5Icons::delete_(),
                .attributes = {
                    "data-class"_attr = "file-tracking-trash",
                    "data-ft-action"_attr = action,
                    type = "button",
                    title = tooltip,
                    "aria-label"_attr = tooltip,
                    onClick = [&onRemove]() {
                        onRemove();
                    },
                },
                .styleVariant = Snc::StyleVariant::Transparent,
            });
            // clang-format on
        }

        Nui::ElementRenderer groupActions(TrackingActions const& actions, OutlinerGroup group)
        {
            switch (group)
            {
                case OutlinerGroup::Stopped:
                {
                    const auto tooltip = language->get("fileTracking", "removeAllStoppedTooltip");
                    return groupTrash("remove-all-stopped", tooltip, actions.removeAllStopped);
                }
                case OutlinerGroup::Orphaned:
                {
                    const auto tooltip = language->get("fileTracking", "deleteAllOrphansTooltip");
                    return groupTrash("delete-all-orphans", tooltip, actions.deleteAllOrphanedCopies);
                }
                case OutlinerGroup::NeedsYou:
                case OutlinerGroup::Syncing:
                case OutlinerGroup::Watching:
                    break;
            }
            return Nui::nil();
        }

        Nui::ElementRenderer headerEnd(
            TrackingActions const& actions,
            OutlinerGroup group,
            std::shared_ptr<Nui::Observed<std::uint64_t>> const& count
        )
        {
            using namespace Nui::Attributes;
            using Nui::Elements::span;
            namespace Snc = ScriptNuiComponents;

            // clang-format off
            return span{class_ = "file-tracking-group-header-end"}(
                Snc::countBadge(
                    count,
                    Snc::StyleVariant::Transparent,
                    {"data-class"_attr = "file-tracking-group-count"}
                ),
                groupActions(actions, group)
            );
            // clang-format on
        }

        Nui::ElementRenderer groupRows(TrackingStore& store, TrackingActions const& actions, OutlinerGroup group)
        {
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            // clang-format off
            if (group == OutlinerGroup::Orphaned)
            {
                return div{class_ = "file-tracking-group-rows"}(
                    Nui::range(store.orphanMembers()),
                    [&store, &actions](long long, std::shared_ptr<OrphanItem> const& item) -> Nui::ElementRenderer {
                        return orphanRow(store, actions, item);
                    }
                );
            }
            return div{class_ = "file-tracking-group-rows"}(
                Nui::range(store.membersOf(group)),
                [&store, &actions](long long, std::shared_ptr<WatchItem> const& item) -> Nui::ElementRenderer {
                    return watchRow(store, actions, item);
                }
            );
            // clang-format on
        }

        Nui::ElementRenderer outlinerGroup(TrackingStore& store, TrackingActions const& actions, OutlinerGroup group)
        {
            using namespace Nui::Attributes;
            namespace Snc = ScriptNuiComponents;

            const auto count = store.countOf(group);

            // clang-format off
            return Snc::collapsibleSection(
                {
                    .title = titleOf(group),
                    .headerEnd = headerEnd(actions, group, count),
                    .initiallyExpanded = store.groupExpanded(group),
                    .onToggle = [&actions, group](bool expanded) {
                        actions.setGroupExpanded(group, expanded);
                    },
                    .attributes = {
                        "data-ft-group"_attr = Utility::enumToString(group),
                        "data-hidden"_attr = Nui::observe(count).generate([count]() {
                            return count->value() == 0u;
                        }),
                    },
                },
                groupRows(store, actions, group)
            );
            // clang-format on
        }

        Nui::ElementRenderer filterField(TrackingStore& store, TrackingActions const& actions)
        {
            using namespace Nui::Attributes;
            using Nui::Elements::label;
            namespace Snc = ScriptNuiComponents;

            const auto placeholder = language->get("fileTracking", "filterPlaceholder");

            // clang-format off
            return label{class_ = "file-tracking-filter"}(
                Ui5Icons::search(),
                Snc::textInput({
                    .value = store.filter(),
                    .attributes = {
                        "data-ft-filter"_attr = true,
                        placeHolder = placeholder,
                        "aria-label"_attr = placeholder,
                        "autocomplete"_attr = "off",
                        "spellcheck"_attr = "false",
                        onInput = [&actions](Nui::val event) {
                            actions.filter(event["target"]["value"].as<std::string>());
                        },
                    },
                    .dontUpdateValue = true,
                })
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer outliner(TrackingStore& store, TrackingActions const& actions)
    {
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        std::vector<Nui::ElementRenderer> groups;
        groups.reserve(outlinerGroupOrder.size());
        for (const auto group : outlinerGroupOrder)
            groups.push_back(outlinerGroup(store, actions, group));

        // clang-format off
        return div{class_ = "file-tracking-outliner"}(
            filterField(store, actions),
            div{
                class_ = "file-tracking-outliner-list",
                "role"_attr = "listbox",
                "aria-label"_attr = language->get("fileTracking", "outlinerLabel"),
                onKeyDown = [&actions](Nui::WebApi::KeyboardEvent event) {
                    moveSelectionByKey(actions, event);
                },
            }(
                Nui::range(std::move(groups)),
                [](long long, Nui::ElementRenderer const& group) -> Nui::ElementRenderer {
                    return group;
                }
            )
        );
        // clang-format on
    }
}
