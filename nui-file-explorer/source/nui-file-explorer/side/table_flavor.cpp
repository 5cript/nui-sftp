#include <nui-file-explorer/side/table_flavor.hpp>
#include <nui-file-explorer/side.hpp>

#include <nui/frontend/api/json.hpp>

#include <fmt/ranges.h>

namespace NuiFileExplorer
{
    TableFlavor::TableFlavor(Side& impl, Side& otherSide)
        : FlavorImplementation{impl, otherSide}
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

        auto contextMenu = [this](auto& item)
        {
            return "contextmenu"_event = [this, &item](Nui::val event)
            {
                side_->onContextMenu(&item, event);
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
            style = observe(impl().tableGridTemplateColumns, impl().items).generate([this]() {
                return fmt::format(
                    "grid-template-columns: {};"
                    "grid-template-rows: minmax(40px, min-content) repeat({}, 25px)", fmt::join(impl().tableGridTemplateColumns.value(), " "), impl().items.value().size()
                );
            }),
            "drop"_event = [this](Nui::WebApi::DragEvent dropEvent) {
                onDrop(std::move(dropEvent), std::nullopt);
            },
            allowDrop,
        }(
            div{
                class_ = "nui-file-grid-table-header"
            }(
                makeHeaderCell(SortCriterion::Name, "Name", 0),
                makeHeaderCell(SortCriterion::Size, "Size", 1),
                makeHeaderCell(SortCriterion::Info, "Info", 2),
                makeHeaderCell(SortCriterion::Mtime, "Last Modified", 3)
            ),
            div{
                class_ = "nui-file-grid-table-rows"
            }(
                impl().items.map([this, contextMenu](auto index, auto& item){
                    return div{
                        class_ = item.observeClassRelevant([&item](){
                            const auto searchHighlight = item.searchHighlight();
                            const auto isDropHovered = item.isDropHovered();

                            return fmt::format("nui-file-grid-table-row {} {} {}", item.isSelected() ? "selected" : "",
                                searchHighlight == ItemWithInternals::SearchHighlight::Highlight ? "is-highlighted"
                                : searchHighlight == ItemWithInternals::SearchHighlight::Muted ? "is-muted"
                                : "", isDropHovered ? "drop-hovered" : "");
                        }),
                        onDblClick = [this, &item](Nui::val event){
                            event.call<void>("stopPropagation");
                            side_->closeMenus();
                            impl().model->onActivateItem(item.item);
                        },
                        onClick = [this, &item](Nui::WebApi::MouseEvent event){
                            Nui::WebApi::Console::log("table item click");
                            side_->onItemClicked(item, std::move(event));
                        },
                        reference = [&item](std::weak_ptr<Nui::Dom::BasicElement> ref) {
                            item.element = ref;
                        },
                        draggable = item.observeSelected([&item]() {
                            return item.isSelected() ? "true" : "false";
                        }),
                        "data-index"_attr = std::to_string(index),
                        "dragstart"_event = [this](Nui::WebApi::DragEvent event){
                            Nui::WebApi::Console::log(event.val());
                            event.stopPropagation();

                            auto dataTransferOpt = event.dataTransfer();
                            if (!dataTransferOpt.has_value())
                            {
                                Nui::WebApi::Console::log("Cannot set data transfer opt, because its nullish.");
                                return;
                            }

                            Nui::val info = Nui::val::object();
                            info.set("isLeft", side_->model().isLeft());
                            dataTransferOpt->setData("application/json", Nui::JSON::stringify(info));
                        },
                        "drop"_event = [this, &item](Nui::WebApi::DragEvent event){
                            item.isDropHovered(false);
                            onDrop(std::move(event), item.item);
                        },
                        "dragenter"_event = [&item](Nui::WebApi::DragEvent){
                            if (item.item.isDirectoryLike())
                                item.isDropHovered(true);
                        },
                        "dragleave"_event = [&item](Nui::WebApi::DragEvent){
                            item.isDropHovered(false);
                        }
                    }(
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                            "data-type"_attr = fileTypeToString(item.item.type),
                        }(
                            img{
                                src = item.item.icon,
                                draggable = "false",
                                alt = "???",
                                width = "16",
                                height = "16",
                                style = item.item.type == Item::Type::Directory ? "filter: hue-rotate(120deg)" : "filter: invert(100%) brightness(2)",
                            }(),
                            span{}(item.item.path.filename().string())
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            span{}(Utility::formatBytes(item.item.size))
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            span{}(item.item.lsStyleTypePermsUserGroup())
                        ),
                        div{
                            class_ = "nui-file-grid-table-cell",
                            contextMenu(item),
                        }(
                            span{}(item.item.readableMTime())
                        )
                    );
                })
            )
        );
        // clang-format on
    }
}