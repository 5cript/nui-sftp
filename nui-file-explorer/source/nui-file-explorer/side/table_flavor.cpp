#include "side_impl.hpp"

namespace NuiFileExplorer
{
    Nui::ElementRenderer Side::tableFlavor()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        auto makeTableResizer = []() -> Nui::ElementRenderer
        {
            return div{
                class_ = "nui-file-grid-resizer",
                "mousedown"_event = [](Nui::val event)
                {
                    event.call<void>("stopPropagation");

                    const auto startX = event["clientX"].as<int>();
                    // FIXME: not here!
                    std::vector<int> startWidths = {120, 100, 100, 280};

                    // TODO: ...
                }
            }();
        };

        auto makeSortIndicator = []() -> Nui::ElementRenderer
        {
            return span{class_ = "nui-file-grid-sort-indicator"}(
                span{
                    class_ = "nui-file-grid-arrow nfg-up",
                }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x25B2)),
                span{
                    class_ = "nui-file-grid-arrow nfg-down",
                }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x25BC))
            );
        };

        auto makeHeaderCellSpan = [](std::string const& text) -> Nui::ElementRenderer
        {
            return span{style = "flex-grow: 1;"}(text);
        };

        auto makeOnClick = [this](SortCriterion sortCriterion)
        {
            return [this, sortCriterion](Nui::val event)
            {
                event.call<void>("stopPropagation");

                const auto [previousCriterion, previousAscending] = impl_->sorting.value();
                if (previousCriterion != sortCriterion)
                    impl_->sorting = {sortCriterion, true};
                else
                    impl_->sorting = {sortCriterion, !previousAscending};
                impl_->sortItems();
                impl_->items.modifyNow();
            };
        };

        auto makeHeaderCellClass = [this](SortCriterion sortCriterion)
        {
            return observe(impl_->sorting)
                .generate(
                    [this, sortCriterion]()
                    {
                        if (impl_->sorting.value().first == sortCriterion)
                        {
                            if (impl_->sorting.value().second)
                                return "nui-file-grid-table-header-cell sorted-asc";
                            else
                                return "nui-file-grid-table-header-cell sorted-desc";
                        }
                        return "nui-file-grid-table-header-cell";
                    }
                );
        };

        auto contextMenu = [this](auto const& item)
        {
            return "contextmenu"_event = [this, item = item.item](Nui::val event)
            {
                onContextMenu(item, event);
            };
        };

        // clang-format off
        return Nui::Elements::div{class_ = "nui-file-grid-table"}(
            div{
                class_ = "nui-file-grid-table-header"
            }(
                div{
                    class_ = makeHeaderCellClass(SortCriterion::Name),
                    onClick = makeOnClick(SortCriterion::Name)
                }(
                    makeHeaderCellSpan("Name"),
                    makeSortIndicator(),
                    makeTableResizer()
                ),
                div{
                    class_ = makeHeaderCellClass(SortCriterion::Type),
                    onClick = makeOnClick(SortCriterion::Type)
                }(
                    makeHeaderCellSpan("Type"),
                    makeSortIndicator(),
                    makeTableResizer()
                ),
                div{
                    class_ = makeHeaderCellClass(SortCriterion::Size),
                    onClick = makeOnClick(SortCriterion::Size)
                }(
                    makeHeaderCellSpan("Size"),
                    makeSortIndicator(),
                    makeTableResizer()
                ),
                div{
                    class_ = makeHeaderCellClass(SortCriterion::Atime),
                    onClick = makeOnClick(SortCriterion::Atime)
                }(
                    makeHeaderCellSpan("Modification Date"),
                    makeSortIndicator()
                )
            ),
            div{
                class_ = "nui-file-grid-table-rows"
            }(
                impl_->items.map([this, contextMenu](auto, auto const& item){
                    return div{
                        class_ = observe(item.selected).generate([&item](){
                            if (item.selected->value())
                                return "nui-file-grid-table-row selected";
                            return "nui-file-grid-table-row";
                        }),
                        onDblClick = [this, &item](Nui::val event){
                            event.call<void>("stopPropagation");
                            closeMenus();
                            impl_->model->onActivateItem(item.item);
                        },
                        onClick = [this, &item](Nui::val event){
                            onItemClicked(item, event);
                        }
                    }(
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            img{
                                src = item.item.icon,
                                alt = "???",
                                width = "16",
                                height = "16",
                                style = item.item.type == Item::Type::Directory ? "filter: hue-rotate(120deg)" : "filter: invert(100%) brightness(2)",
                            }(),
                            span{
                            }(item.item.path.filename().string())
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            span{
                            }(Utility::enumToString(item.item.type))
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            span{
                            }(Utility::formatBytes(item.item.size))
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            // TODO: Works but is ugly:
                            span{
                            }(secondsSinceEpochToReadable(item.item.atime))
                        )
                    );
                })
            )
        );
        // clang-format on
    }
}