#include "nui/frontend/element_renderer.hpp"
#include <nui-file-explorer/dropdown_menu.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/event_context.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

#include <fmt/format.h>

namespace NuiFileExplorer
{
    struct DropdownMenu::Implementation
    {
        Nui::Observed<std::vector<std::string>> items;
        std::function<void(std::string const&)> onSelectCallback;
        std::function<void()> onOpen;
        Nui::Observed<bool> isOpen;
        std::string title;

        Implementation(
            std::vector<std::string> items,
            std::function<void(std::string const&)> callback,
            std::function<void()> onOpen,
            std::string title = {}
        )
            : items{std::move(items)}
            , onSelectCallback{std::move(callback)}
            , onOpen{std::move(onOpen)}
            , isOpen{false}
            , title{std::move(title)}
        {}
    };

    DropdownMenu::DropdownMenu(
        std::vector<std::string> items,
        std::function<void(std::string const&)> callback,
        std::function<void()> onOpen,
        std::string title
    )
        : impl_{
              std::make_unique<Implementation>(
                  std::move(items),
                  std::move(callback),
                  std::move(onOpen),
                  std::move(title)
              ),
          }
    {}

    DropdownMenu::~DropdownMenu() = default;
    DropdownMenu::DropdownMenu(DropdownMenu&&) noexcept = default;
    DropdownMenu& DropdownMenu::operator=(DropdownMenu&&) noexcept = default;

    void DropdownMenu::items(const std::vector<std::string>& items)
    {
        impl_->items = items;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    Nui::ElementRenderer DropdownMenu::operator()(std::vector<Nui::Attribute>&& attributes) const
    {
        using namespace std::string_literals;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        attributes.emplace_back(class_ = "nui-file-grid-head-dropdown");
        attributes.emplace_back(
            onClick =
                [this](Nui::val event)
            {
                // open or close the dropdown
                impl_->isOpen = !impl_->isOpen.value();
                if (impl_->isOpen)
                    impl_->onOpen();
                event.call<void>("stopPropagation");
            }
        );

        // clang-format off
        return div{
            std::move(attributes),
        }(
            div{
                class_ = "nui-file-grid-head-dropdown-text"
            }(
                impl_->title
            ),
            div{
                class_ = "nui-file-grid-head-dropdown-button"
            }(
                div{
                    class_ = "nui-file-grid-head-dropdown-button-arrow"
                }()
            ),
            div{
                class_ = "nui-file-grid-head-dropdown-content",
                style = observe(impl_->isOpen).generate([this]() {
                    return fmt::format(
                        "display: {}; top: 100%;",
                        impl_->isOpen.value() ? "block" : "none"
                    );
                })
            }(
                impl_->items.map([this](long long, auto const& item) -> Nui::ElementRenderer {
                    return div{
                        class_ = "nui-file-grid-head-dropdown-content-item",
                        onClick = [this, item](Nui::val event) {
                            impl_->onSelectCallback(item);
                            impl_->isOpen = false;
                            event.call<void>("stopPropagation");
                        }
                    }(
                        item
                    );
                })
            )
        );
        // clang-format on
    }

    void DropdownMenu::open()
    {
        impl_->isOpen = true;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
    void DropdownMenu::close()
    {
        impl_->isOpen = false;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
    bool DropdownMenu::isOpen() const
    {
        return impl_->isOpen.value();
    }
}