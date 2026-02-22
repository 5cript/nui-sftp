#include <frontend/settings/setting_group.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/select.hpp>
#include <svgs/add.hpp>
#include <svgs/delete.hpp>
#include <svgs/navigation-right-arrow.hpp>
#include <svgs/navigation-down-arrow.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <frontend/classes.hpp>

namespace Snc = ScriptNuiComponents;
using namespace std::string_literals;

Nui::ElementRenderer group(SettingGroupParameters&& params)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    try
    {
        auto groupKeyContainer = [currentGroupKey = params.currentGroupKey,
                                     inheritanceBehavior = params.inheritanceBehavior,
                                     groupKeys = params.groupKeys,
                                     addGroup = params.addGroup,
                                     removeGroup = params.removeGroup,
                                     onChangeGroup = params.onChangeGroup]() -> Nui::ElementRenderer
        {
            if (!currentGroupKey)
                return Nui::nil();

            auto addGroupButton = [inheritanceBehavior, currentGroupKey, groupKeys, addGroup]() -> Nui::ElementRenderer
            {
                if (inheritanceBehavior != SettingGroupParameters::InheritanceBehavior::Inheritable)
                    return Nui::nil();

                return Snc::button({
                    .icon = GeneratedSvgs::add(),
                    .attributes =
                        {
                            onClick =
                                [currentGroupKey, groupKeys, addGroup]()
                            {
                                if (!addGroup)
                                {
                                    Log::error("Add group function not set.");
                                    return;
                                }
                                addGroup(*currentGroupKey, *groupKeys);
                            },
                        },
                    .styleVariant = Snc::StyleVariant::Primary,
                });
            };

            auto removeGroupButton =
                [inheritanceBehavior, currentGroupKey, groupKeys, removeGroup]() -> Nui::ElementRenderer
            {
                if (inheritanceBehavior != SettingGroupParameters::InheritanceBehavior::Inheritable)
                    return Nui::nil();

                return Snc::button({
                    .icon = GeneratedSvgs::delete_(),
                    .attributes =
                        {
                            onClick =
                                [currentGroupKey, groupKeys, removeGroup]()
                            {
                                if (!removeGroup)
                                {
                                    Log::error("Remove group function not set.");
                                    return;
                                }
                                removeGroup(*currentGroupKey, *groupKeys);
                            },
                        },
                    .styleVariant = Snc::StyleVariant::Danger,
                });
            };

            return div{
                class_ = "settings-group-key-container"
            }(Nui::Elements::span{}(language->getObserved("settings", "groupKey")),
                Snc::select(
                    Snc::SelectOptions<decltype(*currentGroupKey), decltype(*groupKeys)>{
                        .activeOption = *currentGroupKey,
                        .options = *groupKeys,
                        .attributes =
                            {
                                disabled = observe(*groupKeys)
                                    .generate(
                                        [](std::vector<std::string> const& keys)
                                        {
                                            return keys.empty();
                                        }
                                    ),
                            },
                        .onChange =
                            [currentGroupKey, groupKeys, inheritanceBehavior, onChangeGroup](
                                auto const& newValue, Nui::WebApi::MouseEvent const&
                            )
                        {
                            if (!onChangeGroup)
                            {
                                Log::error("Group key change onChangeGroup function not set.");
                                return;
                            }
                            onChangeGroup(*currentGroupKey, newValue, *groupKeys, inheritanceBehavior);
                        },
                        .activeRenderer = [](auto const& option) -> Nui::ElementRenderer
                        {
                            return Nui::Elements::span{}(
                                observe(option),
                                [&option]() -> std::string
                                {
                                    if (!option.value())
                                        return "</>";
                                    return *option.value();
                                }
                            );
                        },
                        .elementRenderer = [](auto const& option) -> Nui::ElementRenderer
                        {
                            return Nui::Elements::span{}(option);
                        },
                        .dontUpdateValue = true
                    }
                ),
                addGroupButton(),
                removeGroupButton());
        };

        auto makeSessionTypeFilteredDiv =
            [engineTypeFilter = params.engineTypeFilter, engineTypeFilterValue = params.engineTypeFilterValue]()
        {
            if (engineTypeFilter)
            {
                return div{
                    class_ = "settings-group",
                    style = observe(*engineTypeFilter)
                        .generate(
                            [engineTypeFilterValue](Persistence::TerminalEngineType type)
                            {
                                return type == engineTypeFilterValue ? "" : "display: none;";
                            }
                        ),
                };
            }
            return div{
                class_ = "settings-group",
            };
        };

        // clang-format off
        return makeSessionTypeFilteredDiv()(
            div{
                class_ = observe(params.isCollapsed).generate([](bool isCollapsed) {
                    return classes("settings-group-header", isCollapsed ? "collapsed" : "uncollapsed");
                }),
                onClick = [&isCollapsed = params.isCollapsed](){
                    isCollapsed = !*isCollapsed;
                }
            }(
                span{class_ = "settings-group-header-collapse-indicator"}(
                    observe(params.isCollapsed),
                    [](bool isCollapsed){
                        if (isCollapsed)
                            return GeneratedSvgs::navigationrightarrow();
                        else
                            return GeneratedSvgs::navigationdownarrow();
                    }
                ),
                span{class_ = "settings-group-header-title"}(std::move(params.headerTitle))
            ),
            div{
                class_ = observe(params.isCollapsed).generate([](bool isCollapsed) {
                    return classes("settings-group-content", isCollapsed ? "collapsed" : "uncollapsed");
                }),
                style = Nui::Attributes::Style{
                    "padding-top"_style = observe(params.isCollapsed).generate([isCollapsed = &params.isCollapsed, currentGroupKey = params.currentGroupKey]() -> std::string {
                        if (currentGroupKey)
                            return "0px";
                        return *isCollapsed ? "0px" : "8px";
                    }),
                },
            }(
                groupKeyContainer(),
                std::move(params.content)
            )
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::group(): {}", e.what());
        return div{}("Error loading group: "s + e.what());
    }
}