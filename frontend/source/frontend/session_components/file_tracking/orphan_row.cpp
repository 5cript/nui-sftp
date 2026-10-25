#include <frontend/session_components/file_tracking/orphan_row.hpp>
#include <frontend/session_components/file_tracking/status_glyph.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/list_row.hpp>
#include <script-nui-components/style_variant.hpp>

#include <ui5-sap-icons/icons/delete.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <fmt/format.h>

#include <string>
#include <variant>

namespace FileTracking
{
    namespace
    {
        bool holdsInstance(Selection const& selection, Ids::InstanceId const& instanceId)
        {
            auto const* selected = std::get_if<Ids::InstanceId>(&selection);
            return selected != nullptr && *selected == instanceId;
        }

        Nui::ElementRenderer trashButton(TrackingActions const& actions, std::shared_ptr<OrphanItem> const& item)
        {
            using namespace Nui::Attributes;
            namespace Snc = ScriptNuiComponents;

            const auto deleteFormat = language->get("fileTracking", "deleteOrphanTooltip");

            // clang-format off
            const auto tooltip = Nui::observe(item->name).generate([item, deleteFormat]() {
                return fmt::format(fmt::runtime(deleteFormat), item->name.value());
            });
            return Snc::button({
                .icon = Ui5Icons::delete_(),
                .attributes = {
                    "data-class"_attr = "file-tracking-trash",
                    "data-ft-action"_attr = "delete-orphan",
                    type = "button",
                    title = tooltip,
                    "aria-label"_attr = tooltip,
                    onClick = [&actions, item]() {
                        actions.deleteOrphanedCopy(item->instanceId);
                    },
                },
                .styleVariant = Snc::StyleVariant::Transparent,
            });
            // clang-format on
        }
    }

    Nui::ElementRenderer
    orphanRow(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<OrphanItem> const& item)
    {
        using namespace Nui::Attributes;
        namespace Snc = ScriptNuiComponents;

        // clang-format off
        return Snc::listRow({
            .leading = orphanGlyph(),
            .label = item->name,
            .sideText = item->sideText,
            .trailing = trashButton(actions, item),
            .selected = Nui::observe(store.selection()).generate([&store, item]() {
                return holdsInstance(store.selection().value(), item->instanceId);
            }),
            .onActivate = [&actions, item]() {
                actions.select(Selection{item->instanceId});
            },
            .attributes = {
                "data-ft-instance-id"_attr = item->instanceId.value(),
                "data-ft-name"_attr = item->name,
                title = item->name,
            },
        });
        // clang-format on
    }
}
