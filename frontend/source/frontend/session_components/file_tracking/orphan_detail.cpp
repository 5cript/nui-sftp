#include <frontend/session_components/file_tracking/orphan_detail.hpp>
#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>
#include <frontend/session_components/file_tracking/status_glyph.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/style_variant.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <fmt/format.h>

#include <string>

namespace FileTracking
{
    namespace
    {
        Nui::ElementRenderer fact(std::string line)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            return div{class_ = "file-tracking-orphan-fact"}(text{std::move(line)}());
        }

        /**
         * @brief Where the copy came from, when it was downloaded and when its watch ended, and its size.
         */
        Nui::ElementRenderer orphanFacts(SharedData::OrphanedCopy const& copy)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            const auto origin = copy.remotePath
                ? fmt::format(
                      fmt::runtime(language->get("fileTracking", "orphanDownloadedFrom")), copy.remotePath->value()
                  )
                : language->get("fileTracking", "orphanOriginUnknown");

            // clang-format off
            return div{class_ = "file-tracking-orphan-fact-list"}(
                fact(origin),
                copy.downloadedAt
                    ? fact(fmt::format(
                          fmt::runtime(language->get("fileTracking", "orphanDownloadedAt")),
                          localDateTime(*copy.downloadedAt)
                      ))
                    : Nui::nil(),
                copy.endedAt
                    ? fact(fmt::format(
                          fmt::runtime(language->get("fileTracking", "orphanEndedAt")),
                          localDateTime(*copy.endedAt)
                      ))
                    : Nui::nil(),
                fact(fmt::format(fmt::runtime(language->get("fileTracking", "orphanSize")), sizeText(copy.sizeBytes)))
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer orphanDetail(
        [[maybe_unused]] TrackingStore& store,
        TrackingActions const& actions,
        std::shared_ptr<OrphanItem> const& item
    )
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        if (!item)
            return Nui::nil();

        TrackingActions const* actionsPointer = &actions;
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

        // clang-format off
        return div{
            class_ = "file-tracking-detail",
            "data-ft-detail"_attr = "orphan",
            "data-ft-instance-id"_attr = item->instanceId.value(),
        }(
            header{class_ = "file-tracking-detail-header"}(
                div{class_ = "file-tracking-detail-title-row"}(
                    div{class_ = "file-tracking-detail-identity"}(
                        span{
                            class_ = "file-tracking-detail-kind-icon",
                            "aria-hidden"_attr = "true",
                        }(
                            orphanGlyph()
                        ),
                        span{
                            class_ = "file-tracking-detail-name",
                            Nui::Attributes::title = nameText(),
                        }(
                            text{nameText()}()
                        ),
                        span{class_ = "file-tracking-detail-kind"}(
                            text{language->get("fileTracking", "orphanKind")}()
                        )
                    ),
                    div{class_ = "file-tracking-detail-actions"}(
                        div{class_ = "file-tracking-detail-buttons"}(
                            ScriptNuiComponents::button({
                                .text = language->get("fileTracking", "showFolder"),
                                .attributes = {
                                    "data-ft-action"_attr = "show-folder",
                                    onClick = [actionsPointer, item](Nui::val)
                                    {
                                        if (actionsPointer->openLocally)
                                            actionsPointer->openLocally(item->copy.value().directory);
                                    },
                                },
                            }),
                            ScriptNuiComponents::button({
                                .text = language->get("fileTracking", "deleteCopy"),
                                .attributes = {
                                    "data-ft-action"_attr = "delete-orphan",
                                    onClick = [actionsPointer, item](Nui::val)
                                    {
                                        if (actionsPointer->deleteOrphanedCopy)
                                            actionsPointer->deleteOrphanedCopy(item->instanceId);
                                    },
                                },
                                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            })
                        )
                    )
                )
            ),
            div{class_ = "file-tracking-orphan-facts"}(
                Nui::observe(item->copy),
                [item]() -> Nui::ElementRenderer
                {
                    return orphanFacts(item->copy.value());
                }
            ),
            p{class_ = "file-tracking-orphan-explanation"}(
                text{language->get("fileTracking", "orphanExplanation")}()
            )
        );
        // clang-format on
    }
}
