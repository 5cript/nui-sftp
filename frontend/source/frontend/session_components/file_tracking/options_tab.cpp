#include <frontend/session_components/file_tracking/options_tab.hpp>
#include <shared_data/file_tracking/watch_options.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/switch.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>

#include <array>
#include <memory>
#include <string>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief What one switch of the Options tab shows and changes.
         */
        struct OptionSwitch
        {
            SharedData::WatchOption option;
            std::string label;
            std::string description;
            std::shared_ptr<Nui::Observed<bool>> enabled;
            /** @brief Off and disabled on a downloaded copy, which never moves or deletes the server's file. */
            bool neverForCopy;
        };

        std::array<OptionSwitch, 4> optionSwitchesOf(WatchItem const& item)
        {
            return {
                OptionSwitch{
                    .option = SharedData::WatchOption::AutoReupload,
                    .label = language->get("fileTracking", "columnAutoReupload"),
                    .description = language->get("fileTracking", "optionAutoReuploadDescription"),
                    .enabled = item.autoReupload,
                    .neverForCopy = false,
                },
                OptionSwitch{
                    .option = SharedData::WatchOption::MoveRemoteOnLocalMove,
                    .label = language->get("fileTracking", "columnMoveRemote"),
                    .description = language->get("fileTracking", "optionMoveRemoteDescription"),
                    .enabled = item.moveRemote,
                    .neverForCopy = true,
                },
                OptionSwitch{
                    .option = SharedData::WatchOption::DeleteRemoteOnLocalDelete,
                    .label = language->get("fileTracking", "columnDeleteRemote"),
                    .description = language->get("fileTracking", "optionDeleteRemoteDescription"),
                    .enabled = item.deleteRemote,
                    .neverForCopy = true,
                },
                OptionSwitch{
                    .option = SharedData::WatchOption::CheckRemoteChanges,
                    .label = language->get("fileTracking", "columnCheckRemote"),
                    .description = language->get("fileTracking", "optionCheckRemoteDescription"),
                    .enabled = item.checkRemote,
                    .neverForCopy = false,
                },
            };
        }

        Nui::ElementRenderer lockedOptionRow(OptionSwitch const& option)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Attributes::title;
            using Nui::Elements::div;
            using Nui::Elements::span;

            // clang-format off
            return div{
                class_ = "file-tracking-option",
                "data-ft-option"_attr = Utility::enumToString(option.option),
                "data-locked"_attr = true,
                title = language->get("fileTracking", "downloadedCopyOptionHint"),
            }(
                ScriptNuiComponents::switch_({
                    .isChecked = false,
                    .attributes = {
                        role = "switch",
                        "aria-checked"_attr = "false",
                        "aria-label"_attr = option.label,
                        disabled = true,
                    },
                }),
                span{class_ = "file-tracking-option-label"}(option.label),
                span{class_ = "file-tracking-option-description"}(
                    language->get("fileTracking", "optionNeverForCopy")
                )
            );
            // clang-format on
        }

        Nui::ElementRenderer
        optionRow(TrackingActions const& actions, Ids::WatchId const& watchId, OptionSwitch const& option)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;
            using Nui::Elements::span;

            // clang-format off
            return div{
                class_ = "file-tracking-option",
                "data-ft-option"_attr = Utility::enumToString(option.option),
            }(
                ScriptNuiComponents::switch_({
                    .isChecked = option.enabled,
                    .attributes = {
                        role = "switch",
                        "aria-checked"_attr = Nui::observe(option.enabled).generate(
                            [](bool enabled)
                            {
                                return std::string{enabled ? "true" : "false"};
                            }
                        ),
                        "aria-label"_attr = option.label,
                    },
                    .onChange = [&actions, watchId, which = option.option](bool enabled, Nui::WebApi::MouseEvent const&)
                    {
                        if (actions.setOption)
                            actions.setOption(watchId, which, enabled);
                    },
                }),
                span{class_ = "file-tracking-option-label"}(option.label),
                span{class_ = "file-tracking-option-description"}(option.description)
            );
            // clang-format on
        }

        Nui::ElementRenderer optionsBody(TrackingActions const& actions, WatchItem const& item, bool downloadedCopy)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            const auto switches = optionSwitchesOf(item);
            const auto rowOf = [&actions, &item, downloadedCopy](OptionSwitch const& option)
            {
                if (downloadedCopy && option.neverForCopy)
                    return lockedOptionRow(option);
                return optionRow(actions, item.watchId, option);
            };
            const auto copyHint = [downloadedCopy]() -> Nui::ElementRenderer
            {
                if (!downloadedCopy)
                    return Nui::nil();
                // clang-format off
                return div{class_ = "file-tracking-options-hint"}(
                    language->get("fileTracking", "downloadedCopyOptionHint")
                );
                // clang-format on
            };

            // clang-format off
            return div{class_ = "file-tracking-options-body"}(
                div{class_ = "file-tracking-options"}(
                    rowOf(switches[0]),
                    rowOf(switches[1]),
                    rowOf(switches[2]),
                    rowOf(switches[3])
                ),
                copyHint()
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer optionsTab(
        [[maybe_unused]] TrackingStore& store,
        TrackingActions const& actions,
        std::shared_ptr<WatchItem> const& item
    )
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{class_ = "file-tracking-options-tab"}(
            Nui::observe(item->downloadedCopy).generate(
                [&actions, item](bool downloadedCopy) -> Nui::ElementRenderer
                {
                    return optionsBody(actions, *item, downloadedCopy);
                }
            )
        );
        // clang-format on
    }
}
