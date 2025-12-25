#include "side_impl.hpp"

namespace NuiFileExplorer
{
    Nui::ElementRenderer Side::iconFlavor()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div {
            class_ = "nui-file-grid-icons",
            style = Style{
                "grid-template-columns"_style = observe(impl_->iconSize, impl_->iconSpacing).generate([this]() {
                    return "repeat(auto-fill, minmax(" + std::to_string(impl_->iconSize.value() + impl_->iconSpacing.value()) +
                        "px, 1fr))";
                })
            },
            onClick = [this](Nui::val) {
                onUneventfulClick();
            },
            "contextmenu"_event = [this](Nui::val event) {
                onContextMenu(std::nullopt, event);
            }
        }(
            impl_->items.map([this](auto, auto const& item){
                return div{
                    class_ = observe(item.selected).generate([&item](){
                        if (item.selected->value())
                            return "nui-file-grid-item-icons selected";
                        return "nui-file-grid-item-icons";
                    }),
                    onDblClick = [this, &item](Nui::val event){
                        event.call<void>("stopPropagation");
                        closeMenus();
                        impl_->model->onActivateItem(item.item);
                    },
                    "contextmenu"_event = [this, item = item.item](Nui::val event){
                        onContextMenu(item, event);
                    },
                    onClick = [this, &item](Nui::val event){
                        onItemClicked(item, event);
                    }
                }(
                    img{
                        src = item.item.icon,
                        alt = "???",
                        width = observe(impl_->iconSize).generate([this](){
                            return std::to_string(impl_->iconSize.value());
                        }),
                        height = observe(impl_->iconSize).generate([this](){
                            return std::to_string(impl_->iconSize.value());
                        }),
                        style = item.item.type == Item::Type::Directory ? "filter: hue-rotate(120deg)" : "filter: invert(100%) brightness(2)",
                    }(),
                    div{
                    }(item.item.path.filename().string())
                );
            })
        );
        // clang-format on
    }
}