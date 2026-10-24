#include <frontend/session_components/file_tracking/options_tab.hpp>
#include <shared_data/file_tracking/watch_options.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/language.hpp>

#include <script-nui-components/switch.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>

#include <array>
#include <memory>
#include <string>
#include <utility>

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
            /** @brief Disabled on a downloaded copy, the tracker keeps it off there. */
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

        Nui::ElementRenderer
        optionRow(TrackingActions const& actions, std::shared_ptr<WatchItem> const& item, OptionSwitch const& option)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Attributes::title;
            using Nui::Elements::div;
            using Nui::Elements::span;

            const auto lockedNow = [item, neverForCopy = option.neverForCopy]()
            {
                return neverForCopy && item->downloadedCopy.value();
            };
            const auto textWhileLocked = [lockedNow](std::string unlocked, std::string locked)
            {
                return [lockedNow, unlocked = std::move(unlocked), locked = std::move(locked)]()
                {
                    return lockedNow() ? locked : unlocked;
                };
            };

            // clang-format off
            return div{
                class_ = "file-tracking-option",
                "data-ft-option"_attr = Utility::enumToString(option.option),
                "data-locked"_attr = Nui::observe(item->downloadedCopy).generate(lockedNow),
                title = Nui::observe(item->downloadedCopy).generate(
                    textWhileLocked("", language->get("fileTracking", "downloadedCopyOptionHint"))
                ),
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
                        disabled = Nui::observe(item->downloadedCopy).generate(lockedNow),
                    },
                    .onChange = [&actions, watchId = item->watchId, which = option.option](
                        bool enabled,
                        Nui::WebApi::MouseEvent const&
                    )
                    {
                        if (actions.setOption)
                            actions.setOption(watchId, which, enabled);
                    },
                }),
                span{class_ = "file-tracking-option-label"}(option.label),
                span{class_ = "file-tracking-option-description"}(
                    text{Nui::observe(item->downloadedCopy).generate(
                        textWhileLocked(option.description, language->get("fileTracking", "optionNeverForCopy"))
                    )}()
                )
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

        const auto switches = optionSwitchesOf(*item);

        // clang-format off
        return div{class_ = "file-tracking-options-tab"}(
            div{class_ = "file-tracking-options"}(
                optionRow(actions, item, switches[0]),
                optionRow(actions, item, switches[1]),
                optionRow(actions, item, switches[2]),
                optionRow(actions, item, switches[3])
            ),
            div{
                class_ = "file-tracking-options-hint",
                hidden = Nui::observe(item->downloadedCopy).generate(
                    [item]()
                    {
                        return !item->downloadedCopy.value();
                    }
                ),
            }(language->get("fileTracking", "downloadedCopyOptionHint"))
        );
        // clang-format on
    }
}
