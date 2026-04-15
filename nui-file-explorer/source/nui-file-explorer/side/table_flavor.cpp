#include <nui-file-explorer/side/table_flavor.hpp>
#include <nui-file-explorer/side.hpp>

#include <nui/frontend/api/json.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/event_system/event_context.hpp>

#include <fmt/format.h>

namespace NuiFileExplorer
{
    TableFlavor::TableFlavor(Side& side, Side* otherSide)
        : FlavorImplementation{side, otherSide}
        , columnPixelWidths_{std::vector<std::optional<int>>(columnCount, std::nullopt)}
        , lastContainerWidth_{0.0}
        , resizeObserver_{Nui::val::undefined()}
        , mouseMoveAbort_{}
        , window_{Nui::val::global("window")}
    {}

    TableFlavor::~TableFlavor()
    {
        mouseMoveAbort_.abort();
        if (!resizeObserver_.isNull() && !resizeObserver_.isUndefined())
            resizeObserver_.call<void>("disconnect");
    }

    void TableFlavor::initPixelWidths()
    {
        if (columnPixelWidths_.value().front().has_value())
            return;

        const auto elem = tableRef_.lock();
        if (!elem)
            return;

        std::vector<std::optional<int>> widths(columnCount);
        for (int i = 0; i < columnCount; ++i)
        {
            const auto cell = elem->val().call<Nui::val>(
                "querySelector", fmt::format(".nui-file-grid-table-header > div:nth-child({})", i + 1)
            );
            widths[i] = (!cell.isNull() && !cell.isUndefined())
                ? cell.call<Nui::val>("getBoundingClientRect")["width"].as<int>()
                : 100;
        }
        *columnPixelWidths_ = std::move(widths);
        columnPixelWidths_.modify();
        setupResizeObserver();
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    void TableFlavor::setupResizeObserver()
    {
        if (!resizeObserver_.isNull() && !resizeObserver_.isUndefined())
            resizeObserver_.call<void>("disconnect");

        const auto elem = tableRef_.lock();
        if (!elem)
            return;

        resizeObserver_ = Nui::val::global("ResizeObserver")
                              .new_(
                                  Nui::bind(
                                      [this](Nui::val entries)
                                      {
                                          if (entries["length"].as<int>() == 0)
                                              return;
                                          const double newWidth = entries[0]["contentRect"]["width"].as<double>();
                                          if (newWidth <= 0.0)
                                              return;
                                          if (lastContainerWidth_ <= 0.0)
                                          {
                                              lastContainerWidth_ = newWidth;
                                              return;
                                          }
                                          const double delta = newWidth - lastContainerWidth_;
                                          if (delta > 2.0 || delta < -2.0)
                                          {
                                              const double scale = newWidth / lastContainerWidth_;
                                              for (int i = 0; i < columnCount; ++i)
                                              {
                                                  auto& w = columnPixelWidths_.value()[i];
                                                  if (w.has_value())
                                                      w = std::max(40, static_cast<int>(std::round(*w * scale)));
                                              }
                                              lastContainerWidth_ = newWidth;
                                              columnPixelWidths_.modify();
                                              Nui::globalEventContext.executeActiveEventsImmediately();
                                          }
                                      },
                                      std::placeholders::_1
                                  )
                              );
        resizeObserver_.call<void>("observe", elem->val());
    }

    Nui::ElementRenderer TableFlavor::operator()()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using namespace std::string_literals;
        using Nui::Elements::div;
        using Nui::Elements::span;

        const auto makeHeaderCellClass = [this](SortCriterion sortCriterion)
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

        const auto contextMenu = [this](auto& item)
        {
            return "contextmenu"_event = [this, &item](Nui::val event)
            {
                side_->onContextMenu(&item, event);
            };
        };

        // Each header cell handles both resize (drag) and sort (quick release) via a
        // single onMouseDown. A shared dragStarted flag is set once movement exceeds the
        // threshold; on mouseup, sort fires only if no drag occurred.
        const auto makeHeaderCell = [&](
                                        SortCriterion sortCriterion, std::string const& name, int colIdx, bool resizable
                                    ) -> Nui::ElementRenderer
        {
            return div{
                class_ = makeHeaderCellClass(sortCriterion),
                onMouseDown =
                    [this, sortCriterion, colIdx, resizable](Nui::WebApi::MouseEvent downEvent)
                {
                    downEvent.stopPropagation();

                    // Lazily capture actual pixel widths from the DOM on first interaction.
                    // getBoundingClientRect is reliable here since the user is already interacting.
                    initPixelWidths();

                    const int startX = downEvent.clientX();
                    const int startWidth = columnPixelWidths_.value()[colIdx].value_or(100);
                    const auto dragStarted = std::make_shared<bool>(false);

                    mouseMoveAbort_.abort();
                    mouseMoveAbort_ = {};

                    auto options = Nui::val::object();
                    options.set("signal", mouseMoveAbort_.signal().val());

                    window_.call<void>(
                        "addEventListener",
                        "mousemove"s,
                        Nui::bind(
                            [this, colIdx, startX, startWidth, dragStarted, resizable](Nui::val v)
                            {
                                const auto ev = Nui::WebApi::MouseEvent{std::move(v)};
                                const int dx = ev.clientX() - startX;
                                if (resizable && (dx > resizeDragThreshold || dx < -resizeDragThreshold))
                                {
                                    *dragStarted = true;
                                    columnPixelWidths_.value()[colIdx] = std::max(40, startWidth + dx);
                                    columnPixelWidths_.modify();
                                    Nui::globalEventContext.executeActiveEventsImmediately();
                                }
                            },
                            std::placeholders::_1
                        ),
                        options
                    );

                    window_.call<void>(
                        "addEventListener",
                        "mouseup"s,
                        Nui::bind(
                            [this, sortCriterion, dragStarted](Nui::val)
                            {
                                mouseMoveAbort_.abort();
                                if (!*dragStarted)
                                {
                                    const auto [prevCriterion, prevAscending] = impl().sorting.value();
                                    impl().sorting = {
                                        sortCriterion, prevCriterion != sortCriterion ? true : !prevAscending
                                    };
                                    impl().sortItems();
                                    impl().items.modifyNow();
                                }
                            },
                            std::placeholders::_1
                        ),
                        options
                    );
                }
            }(span{class_ = "nui-file-grid-header-cell-text"}(name),
                span{
                    class_ = "nui-file-grid-col-expand-btn",
                    onMouseDown =
                        [](Nui::WebApi::MouseEvent e)
                    {
                        e.stopPropagation();
                    },
                    onClick =
                        [this, colIdx](Nui::val event)
                    {
                        event.call<void>("stopPropagation");
                        initPixelWidths();
                        const auto elem = tableRef_.lock();
                        if (!elem)
                            return;
                        const int containerWidth = elem->val()["clientWidth"].as<int>();
                        const auto& widths = columnPixelWidths_.value();
                        int sum = 0;
                        for (int i = 0; i < columnCount; ++i)
                            sum += widths[i].value_or(100);
                        const int remaining = containerWidth - sum;
                        if (remaining > 0)
                        {
                            columnPixelWidths_.value()[colIdx] = widths[colIdx].value_or(100) + remaining;
                            columnPixelWidths_.modify();
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        }
                    },
                }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x2194)),
                span{class_ = "nui-file-grid-sort-indicator"}(
                    span{
                        class_ = "nui-file-grid-arrow nfg-up"
                    }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x25B2)),
                    span{
                        class_ = "nui-file-grid-arrow nfg-down"
                    }(Nui::val::global("String").call<std::string>("fromCodePoint", 0x25BC))
                ),
                // Decorative resize affordance — interaction is handled by the parent cell
                resizable ? div{class_ = "nui-file-grid-col-resize-handle"}() : Nui::nil());
        };

        // clang-format off
        return Nui::Elements::div{
            class_ = "nui-file-grid-table",
            reference = [this](std::weak_ptr<Nui::Dom::BasicElement> ref) {
                const bool wasExpired = tableRef_.expired();
                tableRef_ = ref;
                if (!ref.expired() && wasExpired)
                {
                    lastContainerWidth_ = 0.0;
                    setupResizeObserver();
                }
            },
            style = observe(columnPixelWidths_).generate([this]()
            {
                const auto& widths = columnPixelWidths_.value();
                if (!widths.front().has_value())
                    return std::string{"grid-template-columns: 1fr max-content max-content max-content;"};

                std::string cols;
                for (int colIdx = 0; colIdx < columnCount; ++colIdx)
                {
                    if (colIdx > 0)
                        cols += ' ';
                    cols += widths[colIdx].has_value() ? fmt::format("{}px", *widths[colIdx]) : "1fr";
                }
                return fmt::format("grid-template-columns: {};", cols);
            }),
            "dragstart"_event = [this](Nui::WebApi::DragEvent event) {
                onDelegatedDragStart(std::move(event));
            },
            "dragover"_event = [this](Nui::WebApi::DragEvent event) {
                onDelegatedDragOver(std::move(event));
            },
            "dragleave"_event = [this](Nui::WebApi::DragEvent event) {
                onDelegatedDragLeave(std::move(event));
            },
            "drop"_event = [this](Nui::WebApi::DragEvent event) {
                onDelegatedDrop(std::move(event));
            },
        }(
            div{class_ = "nui-file-grid-table-header"}(
                makeHeaderCell(SortCriterion::Name, "Name", 0, true),
                makeHeaderCell(SortCriterion::Size, "Size", 1, true),
                makeHeaderCell(SortCriterion::Info, "Info", 2, true),
                makeHeaderCell(SortCriterion::Mtime, "Last Modified", 3, true)
            ),
            div{class_ = "nui-file-grid-table-rows"}(
                Nui::range(impl().items).before(
                    // Loading placeholder — first table row, glassy styling. Visibility flipped
                    // by the side's debounced loading hint so fast nav doesn't flash this in.
                    div{
                        class_ = observe(impl().showLoadingHint).generate([this]() {
                            return impl().showLoadingHint.value()
                                ? "nui-file-grid-table-row nui-file-grid-loading-placeholder visible"
                                : "nui-file-grid-table-row nui-file-grid-loading-placeholder";
                        }),
                    }(
                        div{class_ = "nui-file-grid-table-cell"}(
                            div{class_ = "nui-file-grid-loading-placeholder-spinner"}(),
                            span{}("...")
                        ),
                        div{class_ = "nui-file-grid-table-cell"}(),
                        div{class_ = "nui-file-grid-table-cell"}(),
                        div{class_ = "nui-file-grid-table-cell"}()
                    )
                ),
                [this, contextMenu](long long index, auto& item) -> Nui::ElementRenderer {
                    const auto pageSizeValue = std::max(1, impl().pageSize.value());
                    const long long pageStart =
                        static_cast<long long>(impl().currentPage.value()) * pageSizeValue;
                    const long long pageEnd = pageStart + pageSizeValue;

                    if (!impl().filterQuery.value().empty())
                    {
                        const auto found = impl().filterMatchPosition.find(index);
                        if (found == impl().filterMatchPosition.end())
                            return Nui::nil();
                        if (found->second < pageStart || found->second >= pageEnd)
                            return Nui::nil();
                    }
                    else if (index < pageStart || index >= pageEnd)
                    {
                        return Nui::nil();
                    }
                    return div{
                        class_ = item.observeClassRelevant([&item](){
                            const auto searchHighlight = item.searchHighlight();
                            const auto isDropHovered = item.isDropHovered();
                            return fmt::format("nui-file-grid-table-row {} {} {}",
                                item.isSelected() ? "selected" : "",
                                searchHighlight == ItemWithInternals::SearchHighlight::Highlight ? "is-highlighted"
                                : searchHighlight == ItemWithInternals::SearchHighlight::Muted ? "is-muted" : "",
                                isDropHovered ? "drop-hovered" : "");
                        }),
                        onDblClick = [this, &item](Nui::val event){
                            event.call<void>("stopPropagation");
                            side_->closeMenus();
                            impl().model->onActivateItem(item.item);
                        },
                        onClick = [this, &item](Nui::WebApi::MouseEvent event){
                            side_->onItemClicked(item, std::move(event));
                        },
                        reference = [&item](std::weak_ptr<Nui::Dom::BasicElement> ref) {
                            item.element = ref;
                        },
                        draggable = "true",
                        "data-index"_attr = std::to_string(index)
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
                                style = item.item.type == Item::Type::Directory
                                    ? "filter: hue-rotate(120deg)"
                                    : "filter: invert(100%) brightness(2)",
                            }(),
                            span{}(item.displayFilename)
                        ),
                        div{class_ = "nui-file-grid-table-cell", contextMenu(item)}(
                            span{}(item.displaySize)
                        ),
                        div{class_ = "nui-file-grid-table-cell", contextMenu(item)}(
                            span{}(item.displayPerms)
                        ),
                        div{class_ = "nui-file-grid-table-cell", contextMenu(item)}(
                            span{}(item.displayMtime)
                        )
                    );
                }
            )
        );
        // clang-format on
    }
}
