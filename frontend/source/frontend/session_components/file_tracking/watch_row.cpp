#include <frontend/session_components/file_tracking/watch_row.hpp>
#include <frontend/session_components/file_tracking/status_glyph.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/list_row.hpp>
#include <script-nui-components/progress_ring.hpp>
#include <script-nui-components/style_variant.hpp>

#include <ui5-sap-icons/icons/delete.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>

namespace FileTracking
{
    namespace
    {
        bool holdsWatch(Selection const& selection, Ids::WatchId const& watchId)
        {
            auto const* selected = std::get_if<Ids::WatchId>(&selection);
            return selected != nullptr && *selected == watchId;
        }

        int percentOf(double fraction)
        {
            return static_cast<int>(std::floor(100.0 * std::clamp(fraction, 0.0, 1.0)));
        }

        std::string syncingText(WatchItem const& item, std::string const& syncingFormat)
        {
            return fmt::format(fmt::runtime(syncingFormat), item.name.value(), percentOf(item.ringFraction->value()));
        }

        Nui::ElementRenderer leading(std::shared_ptr<WatchItem> const& item, std::string const& syncingFormat)
        {
            using namespace Nui::Attributes;
            using Nui::Elements::span;
            namespace Snc = ScriptNuiComponents;

            // clang-format off
            return span{class_ = "file-tracking-row-leading"}(
                Nui::observe(item->status),
                [item, syncingFormat]() -> Nui::ElementRenderer {
                    if (item->status.value() != WatchStatus::Syncing)
                        return statusGlyph(item->status.value());
                    return Snc::progressRing({
                        .fraction = item->ringFraction,
                        .attributes = {
                            "aria-label"_attr = Nui::observe(item->name, item->ringFraction).generate(
                                [item, syncingFormat]() {
                                    return syncingText(*item, syncingFormat);
                                }
                            ),
                        },
                    });
                }
            );
            // clang-format on
        }

        Nui::ElementRenderer trailing(TrackingActions const& actions, std::shared_ptr<WatchItem> const& item)
        {
            using namespace Nui::Attributes;
            using Nui::Elements::span;
            namespace Snc = ScriptNuiComponents;

            const auto removeFormat = language->get("fileTracking", "removeStoppedTooltip");

            // clang-format off
            return span{class_ = "file-tracking-row-actions"}(
                Nui::observe(item->status),
                [&actions, item, removeFormat]() -> Nui::ElementRenderer {
                    if (item->status.value() != WatchStatus::Stopped)
                        return Nui::nil();
                    const auto tooltip = Nui::observe(item->name).generate([item, removeFormat]() {
                        return fmt::format(fmt::runtime(removeFormat), item->name.value());
                    });
                    return Snc::button({
                        .icon = Ui5Icons::delete_(),
                        .attributes = {
                            "data-class"_attr = "file-tracking-trash",
                            "data-ft-action"_attr = "remove-stopped",
                            type = "button",
                            title = tooltip,
                            "aria-label"_attr = tooltip,
                            onClick = [&actions, item]() {
                                actions.removeStopped(item->watchId);
                            },
                        },
                        .styleVariant = Snc::StyleVariant::Transparent,
                    });
                }
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer
    watchRow(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item)
    {
        using namespace Nui::Attributes;
        namespace Snc = ScriptNuiComponents;

        const auto syncingFormat = language->get("fileTracking", "syncingTooltip");

        // clang-format off
        return Snc::listRow({
            .leading = leading(item, syncingFormat),
            .label = item->name,
            .sideText = item->sideText,
            .trailing = trailing(actions, item),
            .selected = Nui::observe(store.selection()).generate([&store, item]() {
                return holdsWatch(store.selection().value(), item->watchId);
            }),
            .onActivate = [&actions, item]() {
                actions.select(Selection{item->watchId});
            },
            .attributes = {
                "data-ft-watch-id"_attr = item->watchId.value(),
                "data-ft-name"_attr = item->name,
                "data-ft-state"_attr = Nui::observe(item->status).generate([item]() {
                    return Utility::enumToString(item->status.value());
                }),
                title = Nui::observe(item->name, item->status, item->ringFraction).generate(
                    [item, syncingFormat]() {
                        if (item->status.value() == WatchStatus::Syncing)
                            return syncingText(*item, syncingFormat);
                        return item->name.value();
                    }
                ),
            },
        });
        // clang-format on
    }
}
