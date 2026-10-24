#include <frontend/session_components/file_tracking/conflicts_tab.hpp>
#include <frontend/session_components/file_tracking/item_list_box.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/style_variant.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/fragment.hpp>

#include <memory>

namespace FileTracking
{
    namespace
    {
        Nui::ElementRenderer conflictRow(TrackingActions const& actions, ConflictItem const& item)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Attributes::title;
            using Nui::Elements::div;

            const auto conflictId = item.conflict.conflictId;
            const auto answer = [&actions, conflictId](SharedData::ConflictResolution resolution)
            {
                return [&actions, conflictId, resolution]()
                {
                    if (actions.answerConflict)
                        actions.answerConflict(conflictId, resolution);
                };
            };

            // clang-format off
            return div{
                class_ = "file-tracking-item-row file-tracking-conflict-row",
                role = "listitem",
                "data-ft-conflict-id"_attr = conflictId.value(),
                "data-ft-kind"_attr = Utility::enumToString(item.conflict.kind),
                "data-ft-path"_attr = item.conflict.localPath.toUtf8(),
            }(
                div{class_ = "file-tracking-item-row-text"}(
                    div{class_ = "file-tracking-item-row-file", title = item.file}(item.file),
                    div{class_ = "file-tracking-item-row-detail", title = item.line}(item.line)
                ),
                div{class_ = "file-tracking-item-row-actions"}(
                    itemListBoxButton({
                        .text = language->get("fileTracking", "replaceOnServer"),
                        .action = "replace",
                        .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                        .onPress = answer(SharedData::ConflictResolution::ReplaceOnServer),
                    }),
                    itemListBoxButton({
                        .text = language->get("fileTracking", "keepServers"),
                        .action = "keep",
                        .onPress = answer(SharedData::ConflictResolution::KeepServers),
                    })
                )
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer conflictsTab(
        [[maybe_unused]] TrackingStore& store,
        TrackingActions const& actions,
        std::shared_ptr<WatchItem> const& item
    )
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        const auto answerAll = [&actions, watchId = item->watchId](SharedData::ConflictResolution resolution)
        {
            return [&actions, watchId, resolution]()
            {
                if (actions.answerAllConflicts)
                    actions.answerAllConflicts(watchId, resolution);
            };
        };

        // clang-format off
        return itemListBox(
            {
                .actions = fragment(
                    itemListBoxButton({
                        .text = language->get("fileTracking", "replaceAllOnServer"),
                        .action = "replace-all",
                        .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                        .onPress = answerAll(SharedData::ConflictResolution::ReplaceOnServer),
                    }),
                    itemListBoxButton({
                        .text = language->get("fileTracking", "keepAllServers"),
                        .action = "keep-all",
                        .onPress = answerAll(SharedData::ConflictResolution::KeepServers),
                    })
                ),
                .hint = language->get("fileTracking", "keepAsksAgain"),
                .emptyText = language->get("fileTracking", "noConflicts"),
                .count = item->conflictCount,
                .paging = ItemListBoxPaging{
                    .page = item->conflictPage,
                    .pageCount = item->conflictPageCount,
                    .onPageChange = [&actions, watchId = item->watchId](int page)
                    {
                        if (actions.showConflictPage)
                            actions.showConflictPage(watchId, page);
                    },
                },
                .attributes = {"data-class"_attr = "conflicts"},
            },
            div{class_ = "file-tracking-item-list", role = "list"}(
                Nui::range(item->conflicts),
                [&actions, keepAlive = item](long long, std::shared_ptr<ConflictItem> const& conflict) -> Nui::ElementRenderer
                {
                    return conflictRow(actions, *conflict);
                }
            )
        );
        // clang-format on
    }
}
