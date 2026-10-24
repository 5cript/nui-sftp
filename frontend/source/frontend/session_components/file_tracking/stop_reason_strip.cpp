#include <frontend/session_components/file_tracking/stop_reason_strip.hpp>
#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <frontend/session_components/file_tracking/status_glyph.hpp>
#include <shared_data/file_tracking/watch_stop.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/message_strip.hpp>
#include <script-nui-components/style_variant.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <fmt/format.h>

#include <optional>
#include <string>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief The name of the watch an overlap stop names: as the tracker sent it, else as the panel knows it.
         */
        std::string otherWatchNameOf(TrackingStore const& store, SharedData::WatchStop const& stop)
        {
            if (stop.otherWatchName)
                return *stop.otherWatchName;
            if (!stop.otherWatchId)
                return {};
            if (auto const* entry = store.model().entryOf(*stop.otherWatchId); entry != nullptr)
                return entry->name;
            return {};
        }

        /**
         * @brief Show “Y” for an overlap stop that names the other watch, Open Again for any other stopped copy,
         *        nothing otherwise.
         */
        Nui::ElementRenderer
        stopActions(TrackingStore const& store, TrackingActions const* actions, std::shared_ptr<WatchItem> const& item)
        {
            using namespace Nui::Attributes;

            auto const& stop = item->stop.value();
            if (!stop)
                return Nui::nil();

            if (stop->reason == SharedData::WatchStopReason::Overlap && stop->otherWatchId)
            {
                return ScriptNuiComponents::button({
                    .text = fmt::format(
                        fmt::runtime(language->get("fileTracking", "showOtherWatch")), otherWatchNameOf(store, *stop)
                    ),
                    .attributes =
                        {
                            "data-ft-action"_attr = "show-other-watch",
                            onClick =
                                [actions, otherWatchId = *stop->otherWatchId](Nui::val)
                            {
                                if (actions->showWatch)
                                    actions->showWatch(otherWatchId);
                            },
                        },
                });
            }

            if (!item->downloadedCopy.value())
                return Nui::nil();

            return ScriptNuiComponents::button({
                .text = language->get("fileTracking", "openAgain"),
                .attributes =
                    {
                        "data-ft-action"_attr = "open-again",
                        onClick =
                            [actions, item](Nui::val)
                        {
                            if (actions->openAgain)
                                actions->openAgain(item->watchId);
                        },
                    },
                .styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            });
        }
    }

    Nui::ElementRenderer
    stopReasonStrip(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        if (!item)
            return Nui::nil();

        TrackingStore const* storePointer = &store;
        TrackingActions const* actionsPointer = &actions;

        // clang-format off
        return ScriptNuiComponents::messageStrip({
            .text = Nui::observe(item->stopText).generate(
                [item]()
                {
                    return item->stopText.value();
                }
            ),
            .styleVariant = ScriptNuiComponents::StyleVariant::Regular,
            .icon = statusGlyph(WatchStatus::Stopped),
            .actions = div{class_ = "file-tracking-stop-actions"}(
                Nui::observe(item->stop, item->downloadedCopy),
                [storePointer, actionsPointer, item]() -> Nui::ElementRenderer
                {
                    return stopActions(*storePointer, actionsPointer, item);
                }
            ),
            .attributes = {
                "data-class"_attr = "file-tracking-stop-strip",
                "data-ft-reason"_attr = Nui::observe(item->stop).generate(
                    [item]()
                    {
                        auto const& stop = item->stop.value();
                        return stop ? Utility::enumToString(stop->reason) : std::string{};
                    }
                ),
                hidden = Nui::observe(item->stop).generate(
                    [item]()
                    {
                        return !item->stop.value().has_value();
                    }
                ),
            },
        });
        // clang-format on
    }
}
