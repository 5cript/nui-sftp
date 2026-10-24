#include <frontend/session_components/file_tracking/watch_header.hpp>
#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <shared_data/file_tracking/watch_stop.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/badge.hpp>
#include <script-nui-components/button.hpp>
#include <script-nui-components/style_variant.hpp>
#include <ui5-sap-icons/icons/synchronize.hpp>
#include <ui5-sap-icons/icons/upload.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace FileTracking
{
    namespace
    {
        ScriptNuiComponents::StyleVariant statusVariantOf(WatchStatus status)
        {
            switch (status)
            {
                case WatchStatus::Syncing:
                    return ScriptNuiComponents::StyleVariant::Primary;
                case WatchStatus::Conflicts:
                    return ScriptNuiComponents::StyleVariant::Warning;
                case WatchStatus::Failed:
                    return ScriptNuiComponents::StyleVariant::Danger;
                case WatchStatus::Watching:
                case WatchStatus::Stopped:
                    break;
            }
            return ScriptNuiComponents::StyleVariant::Regular;
        }

        /**
         * @brief A small header button (data-ft-action=@p actionName) that calls @p onActivate.
         */
        Nui::ElementRenderer actionButton(
            std::string text,
            std::string actionName,
            ScriptNuiComponents::StyleVariant styleVariant,
            std::function<void()> onActivate
        )
        {
            using namespace Nui::Attributes;

            return ScriptNuiComponents::button({
                .text = std::move(text),
                .attributes =
                    {
                        "data-ft-action"_attr = std::move(actionName),
                        onClick =
                            [onActivate = std::move(onActivate)](Nui::val)
                        {
                            onActivate();
                        },
                    },
                .styleVariant = styleVariant,
            });
        }

        /**
         * @brief Open Local Item, Reveal on Server and Stop for a running watch, Open Local Item and Remove for a
         *        stopped one.
         */
        Nui::ElementRenderer
        headerButtons(TrackingActions const* actions, std::shared_ptr<WatchItem> const& item, bool stopped)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            auto openLocal = actionButton(
                language->get("fileTracking", "openLocalItem"),
                "open-local",
                ScriptNuiComponents::StyleVariant::Regular,
                [actions, item]()
                {
                    if (actions->openLocally)
                        actions->openLocally(item->entry.value().localPath);
                }
            );

            if (stopped)
            {
                // clang-format off
                return div{class_ = "file-tracking-detail-buttons"}(
                    std::move(openLocal),
                    actionButton(
                        language->get("fileTracking", "removeButton"),
                        "remove",
                        ScriptNuiComponents::StyleVariant::Regular,
                        [actions, item]()
                        {
                            if (actions->removeStopped)
                                actions->removeStopped(item->watchId);
                        }
                    )
                );
                // clang-format on
            }

            // clang-format off
            return div{class_ = "file-tracking-detail-buttons"}(
                std::move(openLocal),
                actionButton(
                    language->get("fileTracking", "revealOnServer"),
                    "reveal-on-server",
                    ScriptNuiComponents::StyleVariant::Regular,
                    [actions, item]()
                    {
                        if (actions->revealOnServer)
                            actions->revealOnServer(item->entry.value().remotePath);
                    }
                ),
                actionButton(
                    language->get("fileTracking", "stopButton"),
                    "stop",
                    ScriptNuiComponents::StyleVariant::Danger,
                    [actions, item]()
                    {
                        if (actions->stop)
                            actions->stop(item->watchId);
                    }
                )
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer watchHeader(
        [[maybe_unused]] TrackingStore& store,
        TrackingActions const& actions,
        std::shared_ptr<WatchItem> const& item
    )
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        if (!item)
            return Nui::nil();

        TrackingActions const* actionsPointer = &actions;
        const auto openAndWatch = language->get("fileExplorer", "contextMenu", "openAndWatch");
        const auto uploadAndWatch = language->get("fileExplorer", "contextMenu", "uploadAndWatch");
        const auto temporaryCopy = language->get("fileTracking", "temporaryCopy");

        const auto nameText = [item]()
        {
            return Nui::observe(item->name)
                .generate(
                    [item]()
                    {
                        return item->name.value();
                    }
                );
        };
        const auto localText = [item, temporaryCopy]()
        {
            return Nui::observe(item->downloadedCopy, item->localPath)
                .generate(
                    [item, temporaryCopy]()
                    {
                        return item->downloadedCopy.value() ? temporaryCopy : item->localPath.value();
                    }
                );
        };
        const auto localPathText = [item]()
        {
            return Nui::observe(item->localPath)
                .generate(
                    [item]()
                    {
                        return item->localPath.value();
                    }
                );
        };
        const auto remoteText = [item]()
        {
            return Nui::observe(item->remotePath)
                .generate(
                    [item]()
                    {
                        return item->remotePath.value();
                    }
                );
        };

        // clang-format off
        return header{class_ = "file-tracking-detail-header"}(
            div{class_ = "file-tracking-detail-title-row"}(
                div{class_ = "file-tracking-detail-identity"}(
                    span{
                        class_ = "file-tracking-detail-kind-icon",
                        "aria-hidden"_attr = "true",
                    }(
                        Nui::observe(item->downloadedCopy),
                        [item]() -> Nui::ElementRenderer
                        {
                            return item->downloadedCopy.value() ? Ui5Icons::synchronize() : Ui5Icons::upload();
                        }
                    ),
                    span{
                        class_ = "file-tracking-detail-name",
                        Nui::Attributes::title = nameText(),
                    }(
                        text{nameText()}()
                    ),
                    span{class_ = "file-tracking-detail-kind"}(
                        text{Nui::observe(item->downloadedCopy).generate(
                            [item, openAndWatch, uploadAndWatch]()
                            {
                                return item->downloadedCopy.value() ? openAndWatch : uploadAndWatch;
                            }
                        )}()
                    ),
                    ScriptNuiComponents::badge({
                        .text = Nui::observe(item->statusText).generate(
                            [item]()
                            {
                                return item->statusText.value();
                            }
                        ),
                        .styleVariant = Nui::observe(item->status).generate(
                            [item](WatchStatus status)
                            {
                                return statusVariantOf(status);
                            }
                        ),
                        .attributes = {
                            "data-ft-status"_attr = Nui::observe(item->status).generate(
                                [item]()
                                {
                                    return Utility::enumToString(item->status.value());
                                }
                            ),
                        },
                    })
                ),
                div{class_ = "file-tracking-detail-actions"}(
                    Nui::observe(item->stop),
                    [actionsPointer, item]() -> Nui::ElementRenderer
                    {
                        return headerButtons(actionsPointer, item, item->stop.value().has_value());
                    }
                )
            ),
            div{class_ = "file-tracking-detail-paths"}(
                span{
                    class_ = "file-tracking-detail-local",
                    Nui::Attributes::title = localPathText(),
                }(
                    text{localText()}()
                ),
                span{
                    class_ = "file-tracking-detail-remote",
                    Nui::Attributes::title = remoteText(),
                }(
                    text{remoteText()}()
                )
            )
        );
        // clang-format on
    }
}
