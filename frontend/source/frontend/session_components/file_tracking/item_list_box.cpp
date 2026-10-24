#include <frontend/session_components/file_tracking/item_list_box.hpp>

#include <script-nui-components/button.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/utility/merge_attributes.hpp>

#include <utility>

namespace FileTracking
{
    Nui::ElementRenderer itemListBox(ItemListBoxOptions options, Nui::ElementRenderer content)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        std::vector<Nui::Attribute> rootAttributes{};
        rootAttributes.reserve(2u);
        rootAttributes.push_back(class_ = "file-tracking-item-list-box");
        if (options.count)
        {
            rootAttributes.push_back(
                "data-empty"_attr = Nui::observe(options.count)
                    .generate(
                        [](std::uint64_t count)
                        {
                            return count == 0u;
                        }
                    )
            );
        }

        // clang-format off
        return div{Nui::mergeAttributes(std::move(rootAttributes), std::move(options.attributes))}(
            div{class_ = "file-tracking-item-list-box-toolbar"}(
                div{class_ = "file-tracking-item-list-box-actions"}(
                    options.actions ? std::move(*options.actions) : Nui::nil()
                ),
                span{class_ = "file-tracking-item-list-box-hint"}(std::move(options.hint))
            ),
            div{class_ = "file-tracking-item-list-box-items"}(
                std::move(content)
            ),
            div{class_ = "file-tracking-item-list-box-empty"}(std::move(options.emptyText))
        );
        // clang-format on
    }

    Nui::ElementRenderer itemListBoxButton(ItemListBoxButtonOptions options)
    {
        using namespace Nui::Attributes;

        std::vector<Nui::Attribute> attributes{};
        attributes.reserve(2u);
        attributes.push_back("data-ft-action"_attr = std::move(options.action));
        attributes.push_back(
            onClick =
                [onPress = std::move(options.onPress)](Nui::val)
            {
                if (onPress)
                    onPress();
            }
        );
        return ScriptNuiComponents::button({
            .text = std::move(options.text),
            .icon = std::move(options.icon),
            .attributes = Nui::mergeAttributes(std::move(attributes), std::move(options.attributes)),
            .styleVariant = options.styleVariant,
        });
    }
}
