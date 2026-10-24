#include <frontend/session_components/file_tracking/item_list_box.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/pagination.hpp>

#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/dom/basic_element.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/utility/merge_attributes.hpp>

#include <string>
#include <utility>

namespace FileTracking
{
    namespace
    {
        using ElementHandle = std::shared_ptr<std::weak_ptr<Nui::Dom::BasicElement>>;

        /**
         * @brief The pagination bar of @p paging. Choosing another page scrolls @p items back to the top.
         */
        Nui::ElementRenderer paginationOf(ItemListBoxPaging paging, ElementHandle items)
        {
            using namespace Nui::Elements;
            using namespace Nui::Attributes;
            using Nui::Elements::div;

            auto* const page = paging.page.get();
            auto* const pageCount = paging.pageCount.get();

            // clang-format off
            return div{class_ = "file-tracking-item-list-box-pagination"}(
                ScriptNuiComponents::pagination({
                    .pageCount = pageCount,
                    .currentPage = page,
                    .onPageChange = [paging = std::move(paging), items = std::move(items)](int chosen)
                    {
                        if (chosen != paging.page->value())
                        {
                            if (const auto element = items->lock())
                                element->val().set("scrollTop", 0);
                        }
                        if (paging.onPageChange)
                            paging.onPageChange(chosen);
                    },
                    .liveLabel = std::nullopt,
                })
            );
            // clang-format on
        }
    }

    Nui::ElementRenderer itemListBox(ItemListBoxOptions options, Nui::ElementRenderer content)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        const auto paged = options.paging && options.paging->page && options.paging->pageCount;
        const auto items = std::make_shared<std::weak_ptr<Nui::Dom::BasicElement>>();

        std::vector<Nui::Attribute> rootAttributes{};
        rootAttributes.reserve(4u);
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
        if (paged)
        {
            const auto asText = [](int number)
            {
                return std::to_string(number);
            };
            rootAttributes.push_back("data-ft-page"_attr = Nui::observe(options.paging->page).generate(asText));
            rootAttributes.push_back(
                "data-ft-page-count"_attr = Nui::observe(options.paging->pageCount).generate(asText)
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
            div{
                class_ = "file-tracking-item-list-box-items",
                reference = [items](std::weak_ptr<Nui::Dom::BasicElement>&& element)
                {
                    *items = std::move(element);
                },
            }(
                std::move(content)
            ),
            paged ? paginationOf(std::move(*options.paging), items) : Nui::nil(),
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
