#include <frontend/settings/subgroup.hpp>

#include <log/log.hpp>

#include <script-nui-components/switch.hpp>

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
        const bool hasHeader = params.engagedStatus || params.groupTitle;
        const auto headerStyle = [&params]() -> std::string
        {
            if (params.engagedStatus)
                return "border: 1px solid #404040; background-color: var(--darker-background)";
            return "";
        }();
        const auto headerClass = [&params]() -> std::string
        {
            if (!params.engagedStatus && params.groupTitle)
                return "settings-subgroup-header settings-subgroup-header-plain";
            return "settings-subgroup-header";
        }();

        // clang-format off
        return div{
            class_ = "settings-subgroup",
        }(
            div{
                class_ = headerClass,
                style = headerStyle
            }(
                // switch to enable/disable entire subgroup:
                [engagedStatus = params.engagedStatus, title = std::move(params.groupTitle), onChange = std::move(params.onChange)]() mutable -> Nui::ElementRenderer {
                    if (!title)
                        return Nui::nil();
                    if (!engagedStatus)
                        return span{}(std::move(title).value());

                    return fragment(
                        span{}(std::move(title).value()),
                        ScriptNuiComponents::switch_({
                            .isChecked = *engagedStatus,
                            .onChange = [onChange = std::move(onChange)](bool, Nui::WebApi::MouseEvent const&){
                                onChange();
                            },
                        })
                    );
                }()
            ),
            div{
                class_ = "settings-subgroup-content",
                style = params.engagedStatus ? "margin-top: 20px; padding-top: 32px;" : (hasHeader ? "margin-top: 12px; padding-top: 16px;" : "")
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