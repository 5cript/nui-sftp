#include <frontend/settings/subgroup.hpp>

#include <log/log.hpp>

#include <ui5/components/label.hpp>
#include <ui5/components/switch.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

using namespace std::string_literals;

Nui::ElementRenderer subgroup(SubgroupParameters&& params, Nui::ElementRenderer content)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    if (!params.onChange)
    {
        Log::error("SubgroupParameters.onChange is null");
        throw std::runtime_error("SubgroupParameters.onChange is null");
    }

    try
    {
        // clang-format off
        return div{
            class_ = "settings-subgroup",
        }(
            div{
                class_ = "settings-subgroup-header",
                style = params.engagedStatus ? "border: 1px solid var(--sapContent_ForegroundBorderColor); background-color: var(--darkerBackground)" : ""
            }(
                // switch to enable/disable entire subgroup:
                [engagedStatus = params.engagedStatus, title = std::move(params.groupTitle), onChange = std::move(params.onChange)]() mutable -> Nui::ElementRenderer {
                    if (!engagedStatus || !title)
                        return Nui::nil();

                    return fragment(
                        ui5::label{
                            "design"_prop = "Bold",
                        }(std::move(title).value()),
                        ui5::switch_{
                            "checked"_prop = *engagedStatus,
                            "change"_event = [engagedStatus, onChange = std::move(onChange)](Nui::val event) {
                                *engagedStatus = event["target"]["checked"].as<bool>();
                                onChange();
                            },
                        }()
                    );
                }()
            ),
            div{
                class_ = "settings-subgroup-content",
                style = params.engagedStatus ? "margin-top: 20px; padding-top: 32px;" : ""
            }(
                std::move(content)
            )
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::subgroup(): {}", e.what());
        return div{}("Error loading subgroup: "s + e.what());
    }
}