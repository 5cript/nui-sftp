#include <nui-file-explorer/side.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/api/drag_event.hpp>
#include <nui/frontend/api/dom_rect.hpp>

#include <utility/enum_string_convert.hpp>
#include <utility/format_bytes.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <rapidfuzz/fuzz.hpp>

#include <script-nui-components/button.hpp>
#include <ui5-sap-icons/icons/show.hpp>
#include <ui5-sap-icons/icons/hide.hpp>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    namespace
    {
        std::vector<std::string> tokenize(std::string const& input)
        {
            std::vector<std::string> result;
            std::string currentToken;
            for (char c : input)
            {
                if (std::isalnum(c))
                {
                    currentToken.push_back(std::tolower(c));
                }
                else if (std::isspace(c) || std::ispunct(c))
                {
                    if (!currentToken.empty())
                    {
                        result.push_back(currentToken);
                        currentToken.clear();
                    }
                }
            }
            if (!currentToken.empty())
                result.push_back(currentToken);
            return result;
        }
    }

    Side::Side(SideSettings settings, std::unique_ptr<ISideModel> model)
        : impl_(std::make_unique<SideImplementation>(std::move(settings), std::move(model)))
        , iconFlavor_{}
        , tableFlavor_{}
    {}
    void Side::initialize(Side& otherSide)
    {
        iconFlavor_ = std::make_unique<IconFlavor>(*this, otherSide);
        tableFlavor_ = std::make_unique<TableFlavor>(*this, otherSide);
        impl_->otherSide = &otherSide;
    }
    Side::~Side() = default;
    Side::Side(Side&&) = default;
    Side& Side::operator=(Side&&) = default;

    void Side::processKeyboardEvent(Nui::WebApi::KeyboardEvent event)
    {
        auto actionWasTaken = Nui::ScopeExit(
            [&event]() noexcept
            {
                event.stopPropagation();
                event.preventDefault();
            }
        );

        if (event.key() == "F5")
        {
            closeMenus();
            impl_->model->onRefresh();
            return;
        }
        if (event.key() == "F2")
        {
            const auto selectedItems = this->selectedItems();
            if (selectedItems.size() == 1)
                impl_->model->onRename(selectedItems.front());
            return;
        }
        if (event.key() == "Delete")
        {
            const auto selectedItems = this->selectedItems();
            if (!selectedItems.empty())
                impl_->model->onDelete(selectedItems);
            return;
        }
        if (event.key() == "Enter")
        {
            const auto selectedItems = this->selectedItems();
            if (selectedItems.size() == 1)
                impl_->model->onActivateItem(selectedItems.front());
            return;
        }
        if (event.key() == "Backspace")
        {
            impl_->model->goBack();
            return;
        }
        if (event.key() == "h" && event.ctrlKey())
        {
            impl_->showHiddenFiles = !impl_->showHiddenFiles.value();
            if (impl_->settings.onShowHiddenFilesChanged)
                impl_->settings.onShowHiddenFilesChanged(impl_->showHiddenFiles.value());
            updateItems(true, false);
            return;
        }
        if (event.key() == "c" && event.ctrlKey())
        {
            const auto selectedItems = this->selectedItems();
            if (!selectedItems.empty())
                impl_->copiedFiles = selectedItems;
            return;
        }
        if (event.key() == "v" && event.ctrlKey())
        {
            if (impl_->otherSide)
            {
                const auto& copiedFiles = impl_->otherSide->impl_->copiedFiles;
                if (!copiedFiles.empty())
                {
                    // If only a single item is selected and its a directory, set that as the target subdir for the
                    // transfer. Otherwise, transfer to the current directory.
                    std::optional<std::filesystem::path> targetSubdir = std::nullopt;
                    auto selectedItems = this->selectedItems();
                    if (selectedItems.size() == 1 && selectedItems.front().isDirectory())
                        targetSubdir = selectedItems.front().path.filename();
                    impl_->otherSide->model().onTransfer(copiedFiles, targetSubdir);
                }
            }
        }

        if (!impl_->selectionManager.onKeyboardEvent(event))
            actionWasTaken.disarm();
    }

    Nui::ElementRenderer Side::operator()()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div {
            class_ = "nui-file-grid-side",
            reference = impl_->sideElement,
            style = [this]() -> std::optional<std::string> {
                if (model().isLeft())
                    return "border-right: 1px solid var(--nui-file-grid-border-color);";
                return std::nullopt;
            }(),
            "keydown"_event = [this](Nui::WebApi::KeyboardEvent event) {
                processKeyboardEvent(event);
            },
            tabIndex = 0,
        }(
            pathBarSuggestions(),
            [this]() -> Nui::ElementRenderer {
                if (impl_->settings.pathBarOnTop)
                    return pathBar();
                return Nui::nil();
            }(),
            headMenu(),
            div{
                class_ = "nui-file-grid-side-content",
                style = [this]() -> std::string {
                    if (model().isLeft())
                        return "padding-right: 1px;";
                    return "padding-left: 1px;";
                }(),
                reference = [this](std::weak_ptr<Nui::Dom::BasicElement> const& ref) {
                    impl_->scrollContainer = ref;
                }
            }(
                contextMenu(),
                div{
                    style = "width: 100%; height: 100%",
                    "contextmenu"_event = [this](Nui::val event) {
                        onContextMenu(nullptr, event);
                    }
                }(
                    observe(impl_->flavor),
                    [this]() -> Nui::ElementRenderer {
                        if (impl_->flavor.value() == Flavor::Icons)
                            return (*iconFlavor_)();
                        if (impl_->flavor.value() == Flavor::Table)
                            return (*tableFlavor_)();
                        return div{}();
                    }
                )
            ),
            [this]() -> Nui::ElementRenderer {
                if (!impl_->settings.pathBarOnTop)
                    return pathBar();
                return Nui::nil();
            }()
        );
        // clang-format on
    }

    ISideModel& Side::model()
    {
        return *impl_->model;
    }

    void Side::updateItems(bool sorted, bool reapplySelection)
    {
        const auto& items = impl_->model->items();

        const auto selectedPaths = impl_->selectionManager.selectedPaths();
        impl_->selectionManager.loseTrackToAllSelections();

        std::set<std::filesystem::path> pathSet;

        impl_->items.value().clear();
        const bool showHidden = impl_->showHiddenFiles.value();
        auto visibleItems = items |
            std::views::filter(
                [showHidden](auto const& item)
                {
                    const auto filename = item.path.filename().string();
                    return showHidden || filename.empty() || filename[0] != '.' || filename == "..";
                }
            );
        for (auto const& item : visibleItems)
            pathSet.insert(item.path);
        std::ranges::transform(
            visibleItems,
            std::back_inserter(impl_->items.value()),
            [](auto const& item)
            {
                return ItemWithInternals{item};
            }
        );
        if (sorted)
            impl_->sortItems();

        impl_->items.modifyNow();

        // TODO: expensive, also do I want this?
        // reapply selection:
        if (reapplySelection && !selectedPaths.empty())
        {
            std::set<std::filesystem::path> diff;
            std::set_intersection(
                pathSet.begin(),
                pathSet.end(),
                selectedPaths.begin(),
                selectedPaths.end(),
                std::inserter(diff, diff.begin())
            );
            for (auto const& path : diff)
                impl_->selectionManager.select(path);
        }

        // reapply search
        if (auto searchTextBox = impl_->searchTextBox.lock(); searchTextBox)
        {
            const auto query = searchTextBox->val()["value"].as<std::string>();
            if (!query.empty())
                search(query);
        }

        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    void Side::flavor(Flavor value)
    {
        impl_->flavor = value;
        impl_->selectionManager.setFlavor(value);
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
    Flavor Side::flavor() const
    {
        return impl_->flavor.value();
    }

    void Side::iconSize(unsigned int value)
    {
        impl_->iconSize = value;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
    unsigned int Side::iconSize() const
    {
        return impl_->iconSize.value();
    }
    void Side::iconSpacing(unsigned int value)
    {
        impl_->iconSpacing = value;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
    unsigned int Side::iconSpacing() const
    {
        return impl_->iconSpacing.value();
    }

    void Side::onItemClicked(ItemWithInternals const& item, Nui::WebApi::MouseEvent event)
    {
        event.stopPropagation();
        closeMenus();

        impl_->selectionManager.onItemClicked(item, event);
    }

    void Side::onUneventfulClick()
    {
        impl_->pathBoxSuggestions.clear();
        if (!closeMenus())
            impl_->selectionManager.deselectAll();
    }

    std::vector<Item> Side::selectedItems() const
    {
        std::vector<Item> result{};
        for (auto const& item : impl_->items.value())
        {
            if (item.isSelected() && item.isSelectable())
                result.push_back(item.item);
        }
        return result;
    }

    bool Side::closeMenus()
    {
        impl_->newItemMenu.close();
        impl_->sortMenu.close();
        impl_->viewMenu.close();

        if (const auto menu = impl_->contextMenuView.lock(); menu)
        {
            bool wasOpen = menu->val()["style"]["display"].as<std::string>() != "none";
            menu->val()["style"].set("display", "none");
            return wasOpen;
        }
        return false;
    }

    std::pair<int /*x*/, int /*y*/> Side::calculateContextMenuPosition(Nui::val const& event)
    {
        using namespace std::string_literals;
        Nui::WebApi::Console::log(event);

        const auto menu = impl_->contextMenuView.lock();
        if (!menu)
            return {};

        const auto side = impl_->sideElement.lock();
        if (!side)
            return {};

        // its a pointer event, but pointer event derives from mouse event and mouse event has all the interesting
        // props.
        const auto mouseEvent = Nui::WebApi::MouseEvent{event};

        const auto sideViewportCords = Nui::WebApi::DomRect{side->val().call<Nui::val>("getBoundingClientRect")};
        const auto menuRect = Nui::WebApi::DomRect{menu->val().call<Nui::val>("getBoundingClientRect")};

        const auto leftRelativeOffsetToOffsetParent = mouseEvent.clientX() - sideViewportCords.left();
        const auto topRelativeOffsetToOffsetParent = mouseEvent.clientY() - sideViewportCords.top();

        auto top = topRelativeOffsetToOffsetParent + side->val()["offsetTop"].as<double>();
        auto left = leftRelativeOffsetToOffsetParent + side->val()["offsetLeft"].as<double>();

        if (mouseEvent.clientX() + menuRect.width() > sideViewportCords.right())
        {
            // overflow right side
            left -= menuRect.width();
        }
        if (mouseEvent.clientY() + menuRect.height() > sideViewportCords.bottom())
        {
            // overflow bottom side
            top -= menuRect.height();
        }

        return {static_cast<int>(left), static_cast<int>(top)};
    }

    void Side::onContextMenu(ItemWithInternals* item, Nui::val event)
    {
        event.call<void>("stopPropagation");
        event.call<void>("preventDefault");

        const auto menu = impl_->contextMenuView.lock();
        if (!menu)
            return;

        if (item != nullptr)
        {
            Nui::WebApi::Console::log("Context menu item: ", item->item.path.string());
            auto selected = selectedItems();

            // "if it does not appear in the selected list..."
            if (std::find_if(
                    selected.begin(),
                    selected.end(),
                    [&item](auto const& i)
                    {
                        return i.path == item->item.path;
                    }
                ) == selected.end())
            {
                impl_->selectionManager.deselectAll();
                impl_->selectionManager.select(*item);
                impl_->contextMenuClickItems = {item->item};
            }
            else
                impl_->contextMenuClickItems = selected;
        }
        else
        {
            Nui::WebApi::Console::log("Context menu item: none");
            impl_->contextMenuClickItems = selectedItems();
        }
        // filter ".." from context menu click items:
        impl_->contextMenuClickItems.erase(
            std::remove_if(
                impl_->contextMenuClickItems.begin(),
                impl_->contextMenuClickItems.end(),
                [](auto const& item)
                {
                    return item.path.filename() == "..";
                }
            ),
            impl_->contextMenuClickItems.end()
        );

        // display block before calculation, otherwise the rect is 0x0
        menu->val()["style"].set("display", "block");
        auto [left, top] = calculateContextMenuPosition(event);
        menu->val()["style"].set("top", fmt::format("{}px", top));
        menu->val()["style"].set("left", fmt::format("{}px", left));
        return;
    }

    void Side::path(std::filesystem::path const& path)
    {
        model().navigateTo(path);
        // Do NOT call onPathChange here. That is intentional.
    }

    std::filesystem::path Side::path()
    {
        return model().currentPath().value();
    }

    std::vector<std::filesystem::path> Side::selectedPaths() const
    {
        std::vector<Item> selectedItems = this->selectedItems();
        std::vector<std::filesystem::path> result(selectedItems.size());
        std::transform(
            selectedItems.begin(),
            selectedItems.end(),
            result.begin(),
            [](auto const& item)
            {
                return item.path;
            }
        );
        return result;
    }

    Nui::ElementRenderer Side::contextMenu()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{
            reference = impl_->contextMenuView,
            class_ = "nui-file-grid-context-menu",
        }(
            div{
                class_ = "nui-file-grid-context-menu-item",
                onClick = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    closeMenus();
                    if (impl_->contextMenuClickItems.empty())
                        impl_->model->onError("No items selected"s);
                    impl_->model->onTransfer(impl_->contextMenuClickItems, std::nullopt);
                    impl_->contextMenuClickItems = {};
                }
            }(
                "Transfer"
            ),
            div{
                class_ = "nui-file-grid-context-menu-item",
                onClick = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    closeMenus();
                    if (impl_->contextMenuClickItems.empty())
                        impl_->model->onError("No items selected"s);
                    impl_->model->onDelete(impl_->contextMenuClickItems);
                    impl_->contextMenuClickItems = {};
                }
            }(
                "Delete"
            ),
            div{
                class_ = "nui-file-grid-context-menu-item",
                onClick = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    closeMenus();
                    const auto& items = impl_->contextMenuClickItems;
                    if (items.empty())
                        impl_->model->onError("No items selected"s);
                    if (items.size() > 1) {
                        impl_->model->onError("Cannot rename multiple items at once"s);
                    } else if (items.size() == 1) {
                        impl_->model->onRename(items.front());
                    }
                    impl_->contextMenuClickItems = {};
                }
            }(
                "Rename"
            ),
            div{
                class_ = "nui-file-grid-context-menu-item",
                onClick = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    closeMenus();
                    auto const& items = impl_->contextMenuClickItems;
                    if (items.empty())
                        impl_->model->onError("No items selected"s);
                    if (items.size() > 1) {
                        impl_->model->onError("Cannot view properties of multiple items at once"s);
                    } else if (items.size() == 1) {
                        impl_->model->onProperties(items.front());
                    }
                    impl_->contextMenuClickItems = {};
                }
            }(
                "Properties"
            )
        );
        // clang-format on
    }

    Nui::ElementRenderer Side::headMenu()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        namespace Snc = ScriptNuiComponents;

        // clang-format off
        return div {
            class_ = "nui-file-grid-head"
        }(
            impl_->newItemMenu(),
            impl_->sortMenu(),
            impl_->viewMenu(),
            div{}(
                observe(impl_->showHiddenFiles),
                [this]() -> Nui::ElementRenderer {
                    return Snc::button({
                        .icon = impl_->showHiddenFiles.value() ? Ui5Icons::show() : Ui5Icons::hide(),
                        .attributes = {
                            onClick = [this]() {
                                impl_->showHiddenFiles = !impl_->showHiddenFiles.value();
                                if (impl_->settings.onShowHiddenFilesChanged)
                                    impl_->settings.onShowHiddenFilesChanged(impl_->showHiddenFiles.value());
                                updateItems(true, false);
                            }
                        }
                    });
                }
            ),
            filter()
        );
        // clang-format on
    }

    void Side::onPathBoxSuggestionHit(std::filesystem::path const& path)
    {
        impl_->model->navigateTo(path);
        if (auto box = impl_->pathBoxElement.lock(); box)
        {
            box->val().call<void>("focus");
        }
        impl_->pathBoxSuggestions.clear();
    }

    Nui::ElementRenderer Side::pathBarSuggestions()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return div{
            class_ = [this](){
                if (impl_->settings.pathBarOnTop)
                    return "nui-file-explorer-suggestions-overlay top";
                return "nui-file-explorer-suggestions-overlay bottom";
            }(),
            style = observe(impl_->pathBoxSuggestions).generate([this](){
                return fmt::format("display: {};", impl_->pathBoxSuggestions.value().empty() ? "none" : "flex");
            })
        }(
            impl_->pathBoxSuggestions.map([this](long long index, auto const& hit) {
                const auto submit = [this, hit]() {
                    onPathBoxSuggestionHit(hit);
                };

                return div{
                    class_ = "nui-file-explorer-suggestions-item",
                    tabIndex = -1,
                    reference =
                        [this, index](auto&& weakElement) {
                            impl_->searchResultElements[index] = std::move(weakElement);
                        },
                    onClick = submit,
                    onKeyDown = [this, index, submit](Nui::WebApi::KeyboardEvent event) {
                        event.stopPropagation();

                        const auto code = event.key();

                        if (code == "Enter")
                            return submit();

                        if (code == "Escape")
                        {
                            impl_->pathBoxSuggestions.clear();
                            return;
                        }

                        if (code == "ArrowDown" ||
                            code == "ArrowUp")
                        {
                            auto nextElement = impl_->searchResultElements.find(
                                index + (code == "ArrowDown" ? 1 : -1));
                            if (nextElement == impl_->searchResultElements.end())
                                return;

                            auto element = nextElement->second.lock();
                            if (!element)
                                return;

                            element->val().call<void>("focus");
                            auto options = Nui::val::object();
                            options.set("block", "nearest"s);
                            element->val().call<void>("scrollIntoView", options);
                            event.preventDefault();
                        }
                    },
                }(
                    div{}(hit.string())
                );
            })
        );
        // clang-format on
    }

    Nui::ElementRenderer Side::pathBar()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div {
            class_ = "nui-file-grid-path-bar",
            style = [this](){
                if (impl_->settings.pathBarOnTop)
                    return "border-bottom: 1px solid var(--nui-file-grid-border-color)";
                return "border-top: 1px solid var(--nui-file-grid-border-color)";
            }()
        }(
            div{
                /*refresh*/
                onClick = [this](){
                    impl_->model->onRefresh();
                }
            }(),
            input{
                reference = [this](std::weak_ptr<Nui::Dom::BasicElement> const& ref){
                    impl_->pathBoxElement = ref;
                },
                type = "text",
                "value"_prop = observe(model().currentPath()).generate([this](){
                    return model().currentPath().value().generic_string();
                }),
                "keydown"_event = [this](Nui::WebApi::KeyboardEvent event){
                    event.stopPropagation();
                    if (event.key() == "Tab")
                    {
                        event.preventDefault();
                        if (impl_->pathBoxSuggestions.value().empty())
                            return;

                        if (impl_->pathBoxSuggestions.value().size() == 1)
                        {
                            impl_->model->onPathChange(impl_->pathBoxSuggestions.value().front());
                            return;
                        }

                        auto firstElementIt = impl_->searchResultElements.find(0);
                        if (firstElementIt == impl_->searchResultElements.end())
                            return;
                        auto element = firstElementIt->second.lock();
                        if (!element)
                            return;
                        element->val().call<void>("focus");
                        return;
                    }
                },
                "keyup"_event = [this](Nui::WebApi::KeyboardEvent event){
                    event.stopPropagation();
                    if (event.key() == "Enter")
                    {
                        Nui::WebApi::Console::log("Path box Enter hit");
                        impl_->model->onPathChange(std::filesystem::path{event.target()["value"].as<std::string>()});
                        event.preventDefault();
                        return;
                    }

                    if (event.key() == "Tab")
                    {
                        event.preventDefault();
                        return;
                    }

                    if (event.key() == "Escape")
                    {
                        impl_->pathBoxSuggestions.clear();
                        event.preventDefault();
                        return;
                    }

                    if (event.key() == "ArrowDown" ||
                        event.key() == "ArrowUp")
                    {
                        if (impl_->pathBoxSuggestions.value().empty())
                            return;

                        auto isDownNotUp = event.key() == "ArrowDown";
                        auto firstElementIt = impl_->searchResultElements.find(
                            isDownNotUp ? 0 : static_cast<long long>(impl_->pathBoxSuggestions.value().size() - 1));
                        if (firstElementIt == impl_->searchResultElements.end())
                            return;

                        auto element = firstElementIt->second.lock();
                        if (!element)
                            return;

                        element->val().call<void>("focus");
                        event.preventDefault();
                        return;
                    }

                    const auto inputValue = event.target()["value"].as<std::string>();
                    impl_->model->generatePathBoxSuggestions(
                        std::filesystem::path{inputValue},
                        0,
                        [this](std::vector<std::filesystem::path> const& suggestions){
                            impl_->pathBoxSuggestions = suggestions;
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        }
                    );
                }
            }()
        );
        // clang-format on
    }

    Nui::ElementRenderer Side::filter()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        // clang-format off
        return div{
            class_ = "nui-file-grid-filter"
        }(
            img{
                src = "data:image/svg+xml,%3C%3Fxml version='1.0' %3F%3E%3C!DOCTYPE svg PUBLIC '-//W3C//DTD SVG 1.1//EN' 'http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd'%3E%3Csvg height='512px' id='Layer_1' style='enable-background:new 0 0 512 512;' version='1.1' viewBox='0 0 512 512' width='512px' xml:space='preserve' xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'%3E%3Cpath d='M344.5,298c15-23.6,23.8-51.6,23.8-81.7c0-84.1-68.1-152.3-152.1-152.3C132.1,64,64,132.2,64,216.3 c0,84.1,68.1,152.3,152.1,152.3c30.5,0,58.9-9,82.7-24.4l6.9-4.8L414.3,448l33.7-34.3L339.5,305.1L344.5,298z M301.4,131.2 c22.7,22.7,35.2,52.9,35.2,85c0,32.1-12.5,62.3-35.2,85c-22.7,22.7-52.9,35.2-85,35.2c-32.1,0-62.3-12.5-85-35.2 c-22.7-22.7-35.2-52.9-35.2-85c0-32.1,12.5-62.3,35.2-85c22.7-22.7,52.9-35.2,85-35.2C248.5,96,278.7,108.5,301.4,131.2z'/%3E%3C/svg%3E",
                alt = "Filter",
                style = "filter: invert(100%) sepia(3%) saturate(183%) hue-rotate(281deg) brightness(120%) contrast(100%)",
            }(),
            input{
                type = "text",
                placeHolder = "Search",
                reference = [this](std::weak_ptr<Nui::Dom::BasicElement> const& ref){
                    impl_->searchTextBox = ref;
                },
                "keyup"_event = [this](Nui::val event){
                    event.call<void>("stopPropagation");
                    search(event["target"]["value"].as<std::string>());
                },
                "keydown"_event = [](Nui::val event){
                    event.call<void>("stopPropagation");
                }
            }()
        );
        // clang-format on
    }

    void Side::search(std::string query)
    {
        if (query.empty())
        {
            // Clear all highlights
            for (auto& item : impl_->items.value())
                item.searchHighlight(ItemWithInternals::SearchHighlight::Off);
            return;
        }

        constexpr static double hitScore = 90.;
        constexpr static double minimumScore = 60.;

        Utility::Algorithm::toLowerCaseInplace(query);
        const auto queryTokens = tokenize(query);

        auto const score = [&queryTokens](auto const& tokens)
        {
            double max = 0.;
            std::vector<std::string>::const_iterator maxToken;
            for (auto const& query : queryTokens)
            {
                for (auto i = std::cbegin(tokens), end = std::cend(tokens); i != end; ++i)
                {
                    auto const fuzzScore = rapidfuzz::fuzz::partial_ratio(query, *i);
                    if (fuzzScore >= hitScore)
                        return std::make_pair(fuzzScore, i);
                    auto previousMax = max;
                    max = std::max(max, fuzzScore);
                    if (max > previousMax)
                        maxToken = i;
                }
            }
            return std::make_pair(max, maxToken);
        };

        for (auto& item : impl_->items.value())
        {
            const auto generic = item.item.path.generic_string();
            if (generic.find(query) != std::string::npos)
            {
                item.searchHighlight(ItemWithInternals::SearchHighlight::Highlight);
                continue;
            }
            const auto reachedScore = score(tokenize(generic));
            item.searchHighlight(
                reachedScore.first >= minimumScore ? ItemWithInternals::SearchHighlight::Highlight
                                                   : ItemWithInternals::SearchHighlight::Muted
            );
        }
    }
}