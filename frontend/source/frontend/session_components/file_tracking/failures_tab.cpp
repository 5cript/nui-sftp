#include <frontend/session_components/file_tracking/failures_tab.hpp>
#include <frontend/session_components/file_tracking/item_list_box.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/style_variant.hpp>
#include <ui5-sap-icons/icons/decline.hpp>
#include <ui5-sap-icons/icons/refresh.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/fragment.hpp>

#include <fmt/format.h>

#include <memory>
#include <string>

namespace FileTracking
{
    namespace
    {
        /**
         * @return The tooltip of the reason: the system's or the server's detail and, after more than one attempt, how
         *         often it failed. Empty sets no tooltip.
         */
        std::string reasonTooltip(SharedData::UploadFailure const& failure)
        {
            const auto detail = failure.detail.value_or(std::string{});
            if (failure.attempts <= 1u)
                return detail;
            const auto attempts =
                fmt::format(fmt::runtime(language->get("fileTracking", "failedTimes")), failure.attempts);
            if (detail.empty())
                return attempts;
            return fmt::format("{}\n{}", detail, attempts);
        }

        Nui::ElementRenderer failureRow(TrackingActions const& actions, FailureItem const& item)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Attributes::title;
            using Nui::Elements::div;
            using Nui::Elements::span;

            const auto failureId = item.failure.failureId;

            // clang-format off
            return div{
                class_ = "file-tracking-item-row file-tracking-failure-row",
                role = "listitem",
                "data-ft-failure-id"_attr = failureId.value(),
                "data-ft-reason"_attr = Utility::enumToString(item.failure.reason),
                "data-ft-attempts"_attr = std::to_string(item.failure.attempts),
                "data-ft-path"_attr = item.failure.localPath.toUtf8(),
            }(
                div{class_ = "file-tracking-item-row-text"}(
                    div{class_ = "file-tracking-item-row-file", title = item.file}(item.file),
                    div{class_ = "file-tracking-item-row-detail", title = reasonTooltip(item.failure)}(item.reason)
                ),
                div{class_ = "file-tracking-item-row-actions"}(
                    span{class_ = "file-tracking-item-row-time"}(item.time),
                    itemListBoxButton({
                        .text = language->get("fileTracking", "retry"),
                        .action = "retry",
                        .onPress = [&actions, failureId]()
                        {
                            if (actions.retryFailure)
                                actions.retryFailure(failureId);
                        },
                    }),
                    itemListBoxButton({
                        .icon = Ui5Icons::decline(),
                        .action = "dismiss",
                        .styleVariant = ScriptNuiComponents::StyleVariant::Transparent,
                        .onPress = [&actions, failureId]()
                        {
                            if (actions.dismissFailure)
                                actions.dismissFailure(failureId);
                        },
                        .attributes = {
                            title = language->get("fileTracking", "dismiss"),
                            "aria-label"_attr = fmt::format(
                                fmt::runtime(language->get("fileTracking", "dismissFailureLabel")), item.file
                            ),
                        },
                    })
                )
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer failuresTab(
        [[maybe_unused]] TrackingStore& store,
        TrackingActions const& actions,
        std::shared_ptr<WatchItem> const& item
    )
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        const auto watchId = item->watchId;

        // clang-format off
        return itemListBox(
            {
                .actions = fragment(
                    itemListBoxButton({
                        .text = language->get("fileTracking", "retryAll"),
                        .icon = Ui5Icons::refresh(),
                        .action = "retry-all",
                        .styleVariant = ScriptNuiComponents::StyleVariant::Primary,
                        .onPress = [&actions, watchId]()
                        {
                            if (actions.retryAllFailures)
                                actions.retryAllFailures(watchId);
                        },
                    }),
                    itemListBoxButton({
                        .text = language->get("fileTracking", "dismissAll"),
                        .action = "dismiss-all",
                        .onPress = [&actions, watchId]()
                        {
                            if (actions.dismissAllFailures)
                                actions.dismissAllFailures(watchId);
                        },
                    })
                ),
                .hint = language->get("fileTracking", "notRetriedAutomatically"),
                .emptyText = language->get("fileTracking", "noFailures"),
                .count = item->failureCount,
                .attributes = {"data-class"_attr = "failures"},
            },
            div{class_ = "file-tracking-item-list", role = "list"}(
                Nui::range(item->failures),
                [&actions, keepAlive = item](long long, std::shared_ptr<FailureItem> const& failure) -> Nui::ElementRenderer
                {
                    return failureRow(actions, *failure);
                }
            )
        );
        // clang-format on
    }
}
