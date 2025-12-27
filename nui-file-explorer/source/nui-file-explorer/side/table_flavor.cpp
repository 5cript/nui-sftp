#include <nui-file-explorer/side/table_flavor.hpp>
#include <nui-file-explorer/side.hpp>

namespace NuiFileExplorer
{
    TableFlavor::TableFlavor(Side& impl)
        : FlavorImplementation{impl}
    {}

    Nui::ElementRenderer TableFlavor::operator()()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using namespace std::string_literals;
        using Nui::Elements::div;
        using Nui::Elements::span;

        auto shallHide = [this](int columnIndex)
        {
            return style = observe(impl().tableGridTemplateColumns)
                               .generate(
                                   [this, columnIndex]() -> std::optional<std::string>
                                   {
                                       if (impl().tableGridTemplateColumns.value()[columnIndex] ==
                                           impl().tableGridTemplateColumnsHiddenValue)
                                           return "display: none";
                                       else
                                           return std::nullopt;
                                   }
                               );
        };

        auto makeSortIndicator = [&shallHide](int columnIndex) -> Nui::ElementRenderer
        {
            return span{class_ = "nui-file-grid-sort-indicator", shallHide(columnIndex)}(
                span{
                    class_ = "nui-file-grid-arrow nfg-up",
                }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x25B2)),
                span{
                    class_ = "nui-file-grid-arrow nfg-down",
                }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x25BC))
            );
        };

        auto makeHeaderCellSpan = [&shallHide](std::string const& text, int columnIndex) -> Nui::ElementRenderer
        {
            return span{class_ = "nui-file-grid-header-cell-text", shallHide(columnIndex)}(text);
        };

        auto makeOnClick = [this](SortCriterion sortCriterion)
        {
            return [this, sortCriterion](Nui::val event)
            {
                event.call<void>("stopPropagation");

                const auto [previousCriterion, previousAscending] = impl().sorting.value();
                if (previousCriterion != sortCriterion)
                    impl().sorting = {sortCriterion, true};
                else
                    impl().sorting = {sortCriterion, !previousAscending};
                impl().sortItems();
                impl().items.modifyNow();
            };
        };

        auto makeHeaderCellClass = [this](SortCriterion sortCriterion)
        {
            return observe(impl().sorting)
                .generate(
                    [this, sortCriterion]()
                    {
                        if (impl().sorting.value().first == sortCriterion)
                        {
                            if (impl().sorting.value().second)
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
                side_->onContextMenu(item, event);
            };
        };

        auto eyeCollapser = [this](int columnIndex) -> Nui::ElementRenderer
        {
            return span{
                class_ = "nui-file-grid-eye-collapser",
                onClick = [this, columnIndex](Nui::val event)
                {
                    event.call<void>("stopPropagation");
                    event["target"]["classList"].call<void>("toggle", "hidden"s);

                    const std::string hiddenValue{impl().tableGridTemplateColumnsHiddenValue};
                    impl().tableGridTemplateColumns[columnIndex] =
                        impl().tableGridTemplateColumns.value()[columnIndex] == hiddenValue
                        ? std::string{impl().tableGridTemplateColumnsDefaults[columnIndex]}
                        : hiddenValue;
                },
            }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x1F441));
        };

        auto makeHeaderCell =
            [&](SortCriterion sortCriterion, std::string const& name, int columnIndex) -> Nui::ElementRenderer
        {
            return div{
                class_ = makeHeaderCellClass(sortCriterion), onClick = makeOnClick(sortCriterion)
            }(eyeCollapser(columnIndex), makeHeaderCellSpan(name, columnIndex), makeSortIndicator(columnIndex));
        };

        // clang-format off
        return Nui::Elements::div{
            class_ = "nui-file-grid-table",
            style = observe(impl().tableGridTemplateColumns).generate([this]() {
                std::string gridTemplateColumns = "";
                for (const auto& colWidth : impl().tableGridTemplateColumns.value())
                {
                    if (!gridTemplateColumns.empty())
                        gridTemplateColumns += " ";
                    gridTemplateColumns += colWidth;
                }
                return "grid-template-columns: " + gridTemplateColumns + ";";
            })
        }(
            div{
                class_ = "nui-file-grid-table-header"
            }(
                makeHeaderCell(SortCriterion::Name, "Name", 0),
                makeHeaderCell(SortCriterion::Size, "Size", 1),
                makeHeaderCell(SortCriterion::Info, "Info", 2),
                makeHeaderCell(SortCriterion::Atime, "Last Modified", 3)
            ),
            div{
                class_ = "nui-file-grid-table-rows"
            }(
                impl().items.map([this, contextMenu](auto, auto const& item){
                    return div{
                        class_ = observe(item.selected).generate([&item](){
                            if (item.selected->value())
                                return "nui-file-grid-table-row selected";
                            return "nui-file-grid-table-row";
                        }),
                        onDblClick = [this, &item](Nui::val event){
                            event.call<void>("stopPropagation");
                            side_->closeMenus();
                            impl().model->onActivateItem(item.item);
                        },
                        onClick = [this, &item](Nui::WebApi::MouseEvent event){
                            side_->onItemClicked(item, std::move(event));
                        }
                    }(
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            img{
                                src = item.item.icon,
                                draggable = "false",
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
                            }(Utility::formatBytes(item.item.size))
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            span{
                            }(item.item.lsStyleTypePermsUserGroup())
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            span{}(item.item.readableATime())
                        )
                    );
                })
            )
        );
        // clang-format on
    }
}