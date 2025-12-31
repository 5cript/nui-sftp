#include <nui-file-explorer/side/icon_flavor.hpp>
#include <nui-file-explorer/side.hpp>

#include <nui/frontend/api/json.hpp>

#include <nui/frontend/api/dom_rect.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/abort_signal.hpp>
#include <nui/frontend/api/drag_event.hpp>

using namespace std::string_literals;
using namespace std::chrono_literals;

namespace NuiFileExplorer
{
    IconFlavor::IconFlavor(Side& side, Side& otherSide)
        : FlavorImplementation{side, otherSide}
    {}

    void IconFlavor::onBoxDragMouseMove(Nui::WebApi::MouseEvent event)
    {
        auto grid = gridRef_.lock();
        if (!grid)
            return; // Event fired after unmount?

        auto boxLocked = selectBox_.lock();
        if (!boxLocked)
            return;

        auto scrollContainer = impl().scrollContainer_.lock();
        if (!scrollContainer)
            return;

        auto box = boxLocked->val();
        if (std::chrono::steady_clock::now() - mouseDownTime_ > boxDragMinimumTimeToDifferentiateClick)
        {
            if (box["style"]["display"].as<std::string>() != "block" && boxDragActive_)
                box["style"].set("display", "block"s);
        }

        const auto rect = Nui::WebApi::DomRect{grid->val().call<Nui::val>("getBoundingClientRect")};
        const auto currentX = event.clientX() - rect.left();
        const auto currentY = event.clientY() - rect.top();

        const auto x = std::min(startX_, currentX);
        const auto y = std::min(startY_, currentY);
        const auto w = std::abs(currentX - startX_);
        const auto h = std::abs(currentY - startY_);

        box["style"].set("left", fmt::format("{}px", x));
        box["style"].set("top", fmt::format("{}px", y));
        box["style"].set("width", fmt::format("{}px", w));
        box["style"].set("height", fmt::format("{}px", h));
    }

    void IconFlavor::onBoxDragMouseUp(Nui::WebApi::MouseEvent event)
    {
        auto onExit = Nui::ScopeExit{[this]() noexcept
            {
                boxDragActive_ = false;
                mouseMoveAbort_.abort();
                mouseUpAbort_.abort();
                Nui::globalEventContext.executeActiveEventsImmediately();

                // Hide selection box
                auto boxLocked = selectBox_.lock();
                if (!boxLocked)
                    return;

                auto box = boxLocked->val();
                box["style"].set("display", "none");
            }};

        auto grid = gridRef_.lock();
        if (!grid)
            return; // Event fired after unmount?

        auto boxLocked = selectBox_.lock();
        if (!boxLocked)
            return;

        if (std::chrono::steady_clock::now() - mouseDownTime_ < boxDragMinimumTimeToDifferentiateClick)
            return; // not long enough

        // Now we proceed with selection going through, dont do click events:
        shallClick_ = false;

        // Get box rect before display none
        const auto boxRect = Nui::WebApi::DomRect{boxLocked->val().call<Nui::val>("getBoundingClientRect")};

        if (!event.ctrlKey() || event.shiftKey())
            impl().selectionManager.deselectAll();
        for (auto& item : impl().items.value())
        {
            auto itemElement = item.element.lock();
            if (!itemElement)
                continue;

            const auto itemRect = Nui::WebApi::DomRect{itemElement->val().call<Nui::val>("getBoundingClientRect")};

            const auto intersects =
                !(itemRect.right() < boxRect.left() || itemRect.left() > boxRect.right() ||
                    itemRect.bottom() < boxRect.top() || itemRect.top() > boxRect.bottom());

            if (intersects && item.item.path.filename() != "..")
                impl().selectionManager.select(item);
        }
    }

    void IconFlavor::onBoxDragStart(Nui::WebApi::MouseEvent event)
    {
        event.stopPropagation();
        boxDragActive_ = true;

        mouseDownTime_ = std::chrono::steady_clock::now();

        auto grid = gridRef_.lock();
        if (!grid)
            return; // Event fired after unmount?

        auto box = selectBox_.lock();
        if (!box)
            return;

        auto scrollContainer = impl().scrollContainer_.lock();
        if (!scrollContainer)
            return;

        const auto rect = Nui::WebApi::DomRect{grid->val().call<Nui::val>("getBoundingClientRect")};
        startX_ = event.clientX() - rect.left();
        startY_ = event.clientY() - rect.top();

        selectionBox_ = box->val();
        selectionBox_["style"].set("left", fmt::format("{}px", startX_));
        selectionBox_["style"].set("top", fmt::format("{}px", startY_));
        selectionBox_["style"].set("width", "0px"s);
        selectionBox_["style"].set("height", "0px"s);

        auto options = Nui::val::object();

        mouseMoveAbort_ = {};
        options.set("signal", mouseMoveAbort_.signal().val());

        Nui::val::global("window").call<void>(
            "addEventListener",
            "mousemove"s,
            Nui::bind(
                [this](Nui::val event)
                {
                    onBoxDragMouseMove(Nui::WebApi::MouseEvent{std::move(event)});
                },
                std::placeholders::_1
            ),
            options
        );

        auto options2 = Nui::val::object();
        mouseUpAbort_ = {};
        options2.set("signal", mouseUpAbort_.signal().val());

        Nui::val::global("window").call<void>(
            "addEventListener",
            "mouseup"s,
            Nui::bind(
                [this](Nui::val event)
                {
                    onBoxDragMouseUp(Nui::WebApi::MouseEvent{std::move(event)});
                },
                std::placeholders::_1
            ),
            options2
        );
    }

    Nui::ElementRenderer IconFlavor::operator()()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        if (gridLayoutObserver_)
        {
            gridLayoutObserver_->disconnect();
            gridLayoutObserver_ = {};
        }

        // clang-format off
        return div {
            class_ = "nui-file-grid-icons nui-file-grid-gradient",
            onClick = [this](Nui::WebApi::MouseEvent event) {
                if (shallClick_)
                    side_->onUneventfulClick();

                event.stopPropagation();
            },
            "contextmenu"_event = [this](Nui::val event) {
                side_->onContextMenu(nullptr, event);
            },
            "mousedown"_event = [this](WebApi::MouseEvent event) {
                onBoxDragStart(std::move(event));
                shallClick_ = true;
            },
            "drop"_event = [this](Nui::WebApi::DragEvent dropEvent) {
                onDrop(std::move(dropEvent), std::nullopt);
            },
            allowDrop
        }(
            div{
                class_ = "nui-file-grid-selection-box",
                style = "display: none",
                reference = [this](std::weak_ptr<Nui::Dom::BasicElement> const& ref) {
                    selectBox_ = ref;
                },
            }(),
            div{
                class_ = "nui-file-grid-icons-grid",
                style = Style{
                    "grid-template-columns"_style = observe(impl().iconSize, impl().iconSpacing).generate([this]() {
                        return fmt::format("repeat(auto-fit, {}px)", impl().iconSize.value() + impl().iconSpacing.value());
                    })
                },
                "drop"_event = [this](Nui::WebApi::DragEvent dropEvent) {
                    onDrop(std::move(dropEvent), std::nullopt);
                },
                allowDrop,
                !(reference = [this](std::weak_ptr<Nui::Dom::BasicElement> const& ref) {
                    gridRef_ = ref;

                    Nui::WebApi::Console::log("Setting up resize observer for icon grid.");
                    gridLayoutObserver_ = std::make_unique<Nui::WebApi::ResizeObserver>(
                        [this](std::vector<Nui::WebApi::ResizeObserverEntry> const&, Nui::WebApi::ResizeObserver const& self) {
                            auto grid = gridRef_.lock();
                            if (!grid)
                            {
                                Nui::WebApi::Console::log("Grid gone, disconnecting resize observer.");
                                self.disconnect();
                                return;
                            }

                            const auto styles = Nui::val::global().call<Nui::val>("getComputedStyle", grid->val());
                            const auto columns = styles["gridTemplateColumns"].as<std::string>();
                            const auto rows = styles["gridTemplateRows"].as<std::string>();

                            const auto columnCount = std::count(columns.begin(), columns.end(), ' ') + 1;
                            const auto rowCount = std::count(rows.begin(), rows.end(), ' ') + 1;

                            Nui::WebApi::Console::log("Icon grid resized: ", static_cast<std::uint32_t>(columnCount), " columns, ", static_cast<std::uint32_t>(rowCount), " rows.");

                            impl().selectionManager.setGrid(columnCount, rowCount);
                        }
                    );

                    gridLayoutObserver_->observe(gridRef_.lock()->val());
                })
            }(
                impl().items.map([this](auto index, auto& item){
                    return div{
                        class_ = item.observeClassRelevant([&item](){
                            return fmt::format("nui-file-grid-item-icons {} {} {}", item.isSelected() ? "selected" : "",
                                item.searchHighlighted.value() == ItemWithInternals::SearchHighlight::Highlight ? "is-highlighted"
                                : item.searchHighlighted.value() == ItemWithInternals::SearchHighlight::Muted ? "is-muted"
                                : "", item.isDropHovered.value() ? "drop-hovered" : "");
                        }),
                        onDblClick = [this, &item](Nui::val event){
                            event.call<void>("stopPropagation");
                            side_->closeMenus();
                            impl().model->onActivateItem(item.item);
                        },
                        "contextmenu"_event = [this, &item](Nui::val event){
                            side_->onContextMenu(&item, event);
                        },
                        onClick = [this, &item](Nui::WebApi::MouseEvent event){
                            side_->onItemClicked(item, event);
                        },
                        reference = [&item](std::weak_ptr<Nui::Dom::BasicElement> ref) {
                            item.element = ref;
                        },
                        draggable = item.observeSelected([&item]() {
                            return item.isSelected() ? "true" : "false";
                        }),
                        "mousedown"_event = [&item](Nui::WebApi::MouseEvent event){
                            if (item.isSelected())
                                event.stopPropagation();
                        },
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
                            item.isDropHovered = false;
                            onDrop(std::move(event), item.item);
                        },
                        "dragenter"_event = [&item](Nui::WebApi::DragEvent){
                            if (item.item.type == Item::Type::Directory)
                                item.isDropHovered = true;
                        },
                        "dragleave"_event = [&item](Nui::WebApi::DragEvent){
                            item.isDropHovered = false;
                        },
                        "data-index"_attr = std::to_string(index)
                    }(
                        img{
                            src = item.item.icon,
                            draggable = "false",
                            alt = "???",
                            width = observe(impl().iconSize).generate([this](){
                                return std::to_string(impl().iconSize.value());
                            }),
                            height = observe(impl().iconSize).generate([this](){
                                return std::to_string(impl().iconSize.value());
                            }),
                            style = item.item.type == Item::Type::Directory ? "filter: hue-rotate(120deg)" : "filter: invert(100%) brightness(2)",
                        }(),
                        div{
                        }(item.item.path.filename().string())
                    );
                })
            )
        );
        // clang-format on
    }
}