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
#include <script-nui-components/text_input.hpp>
#include <script-nui-components/pagination.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <ui5-sap-icons/icons/show.hpp>
#include <ui5-sap-icons/icons/hide.hpp>
#include <ui5-sap-icons/icons/refresh.hpp>
#include <ui5-sap-icons/icons/search.hpp>
#include <ui5-sap-icons/icons/menu.hpp>
#include <ui5-sap-icons/icons/synchronize.hpp>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    namespace
    {
        // How long the type-ahead buffer survives between keystrokes. A new keystroke
        // resets this; once it expires the buffer is cleared and the highlight is lifted.
        constexpr int typeAheadIdleTimeoutMs = 750;

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
        , places_{}
    {}
    void Side::initialize(Side* otherSide)
    {
        iconFlavor_ = std::make_unique<IconFlavor>(*this, otherSide);
        tableFlavor_ = std::make_unique<TableFlavor>(*this, otherSide);
        impl_->otherSide = otherSide;

        // Any path change is a navigation start. updateItems() clears the loading state when the
        // listing arrives, so the listener only has to mark the start.
        impl_->currentPathListener = Nui::smartListen(
            impl_->model->currentPath(),
            [this](auto const&) {
                beginNavigationLoading();
            }
        );

        // PgUp/PgDn should align with the pagination step so keyboard paging matches the footer.
        impl_->selectionManager.setPageJumpSize(static_cast<std::size_t>(std::max(1, impl_->pageSize.value())));

        if (impl_->model->placesProvider() || impl_->model->favoritesProvider() || impl_->model->drivesProvider())
        {
            places_ = std::make_unique<Places>(
                *impl_->model,
                [this](std::filesystem::path const& path)
                {
                    impl_->model->navigateTo(path);
                }
            );

            impl_->resizeObserver = Nui::val::global("ResizeObserver")
                                        .new_(
                                            Nui::bind(
                                                [this, aliveWeak = std::weak_ptr<bool>(impl_->alive)](Nui::val entries, Nui::val)
                                                {
                                                    if (!aliveWeak.lock())
                                                        return;
                                                    const auto width = entries[0]["contentRect"]["width"].as<double>();
                                                    const bool wide = width >= 400.0;
                                                    if (wide != impl_->isPlacesWide.value())
                                                    {
                                                        impl_->isPlacesWide = wide;
                                                        Nui::globalEventContext.executeActiveEventsImmediately();
                                                    }
                                                },
                                                std::placeholders::_1,
                                                std::placeholders::_2
                                            )
                                        );
        }
    }
    Side::~Side()
    {
        // Tell the browser to stop delivering entries so no queued callback fires into
        // freed memory. The alive flag below is a belt-and-braces guard for any entry
        // already dispatched before disconnect takes effect.
        if (impl_ && !impl_->resizeObserver.isNull() && !impl_->resizeObserver.isUndefined())
            impl_->resizeObserver.call<void>("disconnect");
        if (impl_ && !impl_->typeAheadTimerHandle.isUndefined() &&
            !impl_->typeAheadTimerHandle.isNull())
        {
            Nui::val::global("clearTimeout")(impl_->typeAheadTimerHandle);
            impl_->typeAheadTimerHandle = Nui::val::undefined();
        }
        if (impl_)
            *impl_->alive = false;
    }
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

        if (impl_->selectionManager.onKeyboardEvent(event))
            return;

        // Type-ahead: a printable single-ASCII-character key, no ctrl/alt/meta. Shift is
        // allowed (so "S" and "s" both work). The selection manager consumed arrow keys /
        // Home / End / PageUp / PageDown / Ctrl+A above, so any remaining single-char key
        // is fair game for type-ahead.
        const auto& key = event.key();
        if (!event.ctrlKey() && !event.altKey() && !event.metaKey() && key.size() == 1)
        {
            const auto c = static_cast<unsigned char>(key[0]);
            if (c >= 0x20 && c < 0x7F)
            {
                handleTypeAheadKey(key);
                return;
            }
        }

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
            reference = [this](std::weak_ptr<Nui::Dom::BasicElement> const& ref) {
                impl_->sideElement = ref;
                if (!impl_->resizeObserver.isNull() && !impl_->resizeObserver.isUndefined())
                {
                    if (auto elem = ref.lock(); elem)
                        impl_->resizeObserver.call<void>("observe", elem->val());
                }
            },
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
            impl_->contextMenuPopup(),
            // placesPanel(),
            div{
                class_ = "nui-file-grid-side-middle"
            }(
                // Inline places panel (wide mode, or always-open toggle)
                placesPanel(),
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
                    div{
                        class_ = "nui-file-grid-side-content-inner",
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
                    ),
                    // Pagination footer lives inside the content column so it only spans the item
                    // area — the places panel sits to the left and is unaffected.
                    div{
                        class_ = "nui-file-grid-side-footer",
                        // Swallow bubbled clicks / mousedown so the FileGrid's outer
                        // onClick → onUneventfulClick → deselectAll doesn't fire, AND so the
                        // browser never shifts focus to a pagination button on mousedown —
                        // keeping focus on the side so PgUp/PgDn continue to flow through the
                        // side's keydown handler.
                        onClick = [](Nui::val event) {
                            event.call<void>("stopPropagation");
                        },
                        onMouseDown = [](Nui::val event) {
                            event.call<void>("preventDefault");
                        },
                    }(
                        observe(impl_->pageCount),
                        [this]() -> Nui::ElementRenderer {
                            if (impl_->pageCount.value() <= 1)
                                return Nui::nil();
                            return ScriptNuiComponents::pagination(ScriptNuiComponents::PaginationOptions{
                                .pageCount = &impl_->pageCount,
                                .currentPage = &impl_->currentPage,
                                .onPageChange = [this](int newPage) {
                                    impl_->currentPage = newPage;
                                    // The items.map render block re-evaluates per-item only when
                                    // the items vector itself signals a change, so trigger a
                                    // re-render so the new page's slice takes effect.
                                    impl_->items.modifyNow();
                                    Nui::globalEventContext.executeActiveEventsImmediately();

                                    // Pull focus back to the side so subsequent PgUp/PgDn land
                                    // on the Side's keydown handler instead of the pagination
                                    // button we just clicked.
                                    if (auto sideEl = impl_->sideElement.lock(); sideEl)
                                        sideEl->val().call<void>("focus");
                                },
                                .liveLabel = std::nullopt,
                            });
                        }
                    )
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

        // New directory listing invalidates any active filter — clear it so items aren't
        // silently hidden after navigation.
        if (!impl_->filterQuery.value().empty())
            impl_->filterQuery = "";

        // Recompute paging derived state from the new items count. Reset to page 0 on every
        // navigation so users always start from the top of a freshly-loaded directory.
        const auto pageSizeValue = std::max(1, impl_->pageSize.value());
        const auto itemCount = impl_->items.value().size();
        const int newPageCount =
            std::max(1, static_cast<int>((itemCount + static_cast<std::size_t>(pageSizeValue) - 1) /
                                         static_cast<std::size_t>(pageSizeValue)));
        if (impl_->pageCount.value() != newPageCount)
            impl_->pageCount = newPageCount;
        if (impl_->currentPage.value() != 0)
            impl_->currentPage = 0;

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

        clearNavigationLoading();

        // Reset scroll on the actual scrolling flavor container so a fresh listing always
        // starts from the top, regardless of where the previous directory was scrolled to.
        if (auto contentEl = impl_->scrollContainer.lock(); contentEl)
        {
            const auto scrollers = contentEl->val().call<Nui::val>(
                "querySelectorAll", std::string{".nui-file-grid-icons, .nui-file-grid-table"}
            );
            const auto count = scrollers["length"].as<int>();
            for (int idx = 0; idx < count; ++idx)
                scrollers[idx].set("scrollTop", 0);
        }

        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    void Side::beginNavigationLoading()
    {
        impl_->isLoading = true;

        // Cancel any previously scheduled hint flip — we want a fresh debounce window.
        if (!impl_->loadingHintTimerHandle.isUndefined() && !impl_->loadingHintTimerHandle.isNull())
        {
            Nui::val::global("clearTimeout")(impl_->loadingHintTimerHandle);
            impl_->loadingHintTimerHandle = Nui::val::undefined();
        }

        impl_->loadingHintTimerHandle = Nui::val::global("setTimeout")(
            Nui::bind(
                [this]() {
                    impl_->loadingHintTimerHandle = Nui::val::undefined();
                    if (impl_->isLoading.value())
                    {
                        impl_->showLoadingHint = true;
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }
                }
            ),
            150
        );
    }

    void Side::clearNavigationLoading()
    {
        if (!impl_->loadingHintTimerHandle.isUndefined() && !impl_->loadingHintTimerHandle.isNull())
        {
            Nui::val::global("clearTimeout")(impl_->loadingHintTimerHandle);
            impl_->loadingHintTimerHandle = Nui::val::undefined();
        }
        impl_->showLoadingHint = false;
        impl_->isLoading = false;
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

    void Side::closePathSuggestions(bool otherSideToo) const
    {
        impl_->pathBoxSuggestions.clear();
        if (otherSideToo && impl_->otherSide)
            impl_->otherSide->closePathSuggestions(false);
    }

    void Side::onUneventfulClick()
    {
        closePathSuggestions(true);
        const auto localCloseResult = closeMenus();
        const auto otherCloseResult = impl_->otherSide ? impl_->otherSide->closeMenus() : false;
        if (!localCloseResult && !otherCloseResult)
        {
            impl_->selectionManager.deselectAll();
            if (impl_->otherSide)
                impl_->otherSide->impl_->selectionManager.deselectAll();
        }
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
        const bool wasAnyOpen = impl_->contextMenuPopup.isOpen() || impl_->newItemMenu.isOpen() ||
            impl_->sortMenu.isOpen() || impl_->viewMenu.isOpen();
        impl_->newItemMenu.close();
        impl_->sortMenu.close();
        impl_->viewMenu.close();
        impl_->contextMenuPopup.close();
        return wasAnyOpen;
    }

    void Side::onContextMenu(ItemWithInternals* item, Nui::val event)
    {
        event.call<void>("stopPropagation");
        event.call<void>("preventDefault");

        std::vector<Item> clickItems;

        if (item != nullptr)
        {
            Nui::WebApi::Console::log("Context menu item: ", item->item.path.string());
            auto selected = selectedItems();

            if (std::find_if(
                    selected.begin(),
                    selected.end(),
                    [&item](auto const& sel)
                    {
                        return sel.path == item->item.path;
                    }
                ) == selected.end())
            {
                impl_->selectionManager.deselectAll();
                impl_->selectionManager.select(*item);
                clickItems = {item->item};
            }
            else
                clickItems = selected;
        }
        else
        {
            Nui::WebApi::Console::log("Context menu item: none");
            clickItems = selectedItems();
        }

        // filter ".." from context menu items
        clickItems.erase(
            std::remove_if(
                clickItems.begin(),
                clickItems.end(),
                [](auto const& itm)
                {
                    return itm.path.filename() == "..";
                }
            ),
            clickItems.end()
        );

        closeMenus();
        if (impl_->otherSide)
            impl_->otherSide->closeMenus();

        auto menuItems = impl_->model->contextMenuItems(clickItems);

        if (impl_->onSynchronize && impl_->otherSide)
        {
            namespace Snc = ScriptNuiComponents;

            const bool thisSideOk = clickItems.size() == 1 && clickItems[0].isDirectoryLike();
            const auto otherSelected = impl_->otherSide->selectedItems();
            const bool otherSideOk = otherSelected.size() == 1 && otherSelected[0].isDirectoryLike();

            menuItems.push_back(Snc::PopupMenu::separator());
            if (thisSideOk && otherSideOk)
            {
                const bool thisIsLocal = impl_->model->isLeft();
                const auto localPath = thisIsLocal ? clickItems[0].fullPath : otherSelected[0].fullPath;
                const auto remotePath = thisIsLocal ? otherSelected[0].fullPath : clickItems[0].fullPath;
                menuItems.push_back(Snc::PopupMenu::item(
                    "Synchronize...",
                    Ui5Icons::synchronize(),
                    [this, localPath, remotePath]() { impl_->onSynchronize(localPath, remotePath); }
                ));
            }
            else
            {
                menuItems.push_back(Snc::PopupMenu::item(
                    "Synchronize...",
                    Ui5Icons::synchronize(),
                    {},
                    /*disabled=*/true,
                    /*shortcut=*/{},
                    "Select exactly one directory on each side to synchronize."
                ));
            }
        }

        impl_->contextMenuPopup.setItems(std::move(menuItems));

        const auto mouseEvent = Nui::WebApi::MouseEvent{event};
        impl_->contextMenuPopup.openAt(mouseEvent.clientX(), mouseEvent.clientY());
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

    void Side::setOnSynchronize(
        std::function<void(std::filesystem::path, std::filesystem::path)> callback
    )
    {
        impl_->onSynchronize = std::move(callback);
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

    Nui::ElementRenderer Side::headMenu()
    {
        using namespace Nui;
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        namespace Snc = ScriptNuiComponents;

        const std::string sideStr = model().isLeft() ? "left" : "right";

        // clang-format off
        return div{
            class_ = "nui-file-grid-head"
        }(
            // Burger button: always visible when a places panel exists
            [this]() -> Nui::ElementRenderer {
                if (!places_)
                    return Nui::nil();
                return Snc::button({
                    .icon = Ui5Icons::menu(),
                    .attributes = {
                        onClick = [this]() {
                            Nui::WebApi::Console::log("Toggling places panel.");
                            if (!impl_->isPlacesOpen.value().has_value())
                                impl_->isPlacesOpen = !impl_->isPlacesWide.value(); // if we don't have a state for open vs. closed, just open it as a popup. Otherwise, toggle the state.
                            else
                                impl_->isPlacesOpen = !*impl_->isPlacesOpen.value();
                        }
                    }
                });
            }(),
            impl_->newItemMenu("New",  "nfe-new-"  + sideStr),
            impl_->sortMenu("Sort", "nfe-sort-" + sideStr),
            impl_->viewMenu("View", "nfe-view-" + sideStr),
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

    Nui::ElementRenderer Side::placesPanel()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        if (!places_)
            return Nui::nil();

        // clang-format off
        return div{
            class_ = "nui-file-grid-places",
            style = observe(impl_->isPlacesOpen, impl_->isPlacesWide).generate([](std::optional<bool> isOpen, bool isWide) {
                const auto decidingBool = isOpen.has_value() ? isOpen.value() : isWide;
                Nui::WebApi::Console::log("Generating places panel style with isOpen={} and isWide={}, decides={}", isOpen, isWide, decidingBool);
                return fmt::format("display: {};", decidingBool ? "block" : "none");
            }),
            onClick = [](Nui::val event) {
                event.call<void>("stopPropagation");
            }
        }(
            (*places_)()
        );
        // clang-format on
    }

    Places* Side::places()
    {
        return places_.get();
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
            ScriptNuiComponents::button({
                .icon = Ui5Icons::refresh(),
                .attributes = {
                    onClick = [this](){
                        impl_->model->onRefresh();
                    }
                }
            }),
            ScriptNuiComponents::textInput({
                .value = "",
                .attributes = {
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
                },
                .dontUpdateValue = true
            })
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
            Ui5Icons::search(),
            ScriptNuiComponents::textInput({
                .value = "",
                .attributes = {
                    placeHolder = "Search",
                    reference = [this](std::weak_ptr<Nui::Dom::BasicElement> const& ref){
                        impl_->searchTextBox = ref;
                    },
                    "keyup"_event = [this](Nui::val event){
                        event.call<void>("stopPropagation");
                        search(event["target"]["value"].as<std::string>());
                    },
                    "keydown"_event = [this](Nui::val event){
                        event.call<void>("stopPropagation");
                        // In paginated mode typing doesn't do anything visually (search()
                        // skips the per-item scan). Enter commits the query as a filter so the
                        // grid re-renders to only matching items.
                        if (event["key"].as<std::string>() == "Enter")
                            applyFilter(event["target"]["value"].as<std::string>());
                    },
                    "blur"_event = [this](Nui::val event){
                        // Same commit path as Enter — when focus leaves the box, treat the
                        // current contents as the filter.
                        applyFilter(event["target"]["value"].as<std::string>());
                    },
                },
                .dontUpdateValue = true,
            })
        );
        // clang-format on
    }

    namespace
    {
        constexpr double kSearchHitScore = 90.;
        constexpr double kSearchMinimumScore = 60.;

        /**
         *  @brief Score how strongly @p itemTokens match any query token. Returns the max
         *         partial_ratio across pairs. Used by both search (highlight) and applyFilter
         *         (hide non-matches) so the two modes agree on what counts as a match.
         */
        double scoreItemAgainstQuery(
            std::vector<std::string> const& itemTokens,
            std::vector<std::string> const& queryTokens
        )
        {
            double max = 0.;
            for (auto const& queryToken : queryTokens)
            {
                for (auto const& token : itemTokens)
                {
                    const auto fuzzScore = rapidfuzz::fuzz::partial_ratio(queryToken, token);
                    if (fuzzScore >= kSearchHitScore)
                        return fuzzScore;
                    max = std::max(max, fuzzScore);
                }
            }
            return max;
        }
    }

    void Side::search(std::string query)
    {
        // Paginated directories: skip the per-item highlight scan on every keystroke. The
        // commit path (Enter / blur) calls applyFilter() instead, which re-renders the visible
        // slice to only matching items.
        if (impl_->pageCount.value() > 1 || !impl_->filterQuery.value().empty())
        {
            if (query.empty() && !impl_->filterQuery.value().empty())
                applyFilter("");
            return;
        }

        if (query.empty())
        {
            for (auto& item : impl_->items.value())
                item.searchHighlight(ItemWithInternals::SearchHighlight::Off);
            return;
        }

        Utility::Algorithm::toLowerCaseInplace(query);
        const auto queryTokens = tokenize(query);

        for (auto& item : impl_->items.value())
        {
            const auto generic = item.item.path.generic_string();
            if (generic.find(query) != std::string::npos)
            {
                item.searchHighlight(ItemWithInternals::SearchHighlight::Highlight);
                continue;
            }
            const auto reachedScore = scoreItemAgainstQuery(tokenize(generic), queryTokens);
            item.searchHighlight(
                reachedScore >= kSearchMinimumScore ? ItemWithInternals::SearchHighlight::Highlight
                                                   : ItemWithInternals::SearchHighlight::Muted
            );
        }
    }

    void Side::handleTypeAheadKey(std::string const& key)
    {
        impl_->typeAheadBuffer += key;

        // (Re)start the idle timer. Any prior pending timeout is cancelled so consecutive
        // keystrokes within the idle window keep the buffer alive.
        if (!impl_->typeAheadTimerHandle.isUndefined() && !impl_->typeAheadTimerHandle.isNull())
            Nui::val::global("clearTimeout")(impl_->typeAheadTimerHandle);

        impl_->typeAheadTimerHandle = Nui::val::global("setTimeout")(
            Nui::bind(
                [this]() {
                    impl_->typeAheadTimerHandle = Nui::val::undefined();
                    clearTypeAhead();
                }
            ),
            typeAheadIdleTimeoutMs
        );

        auto needle = impl_->typeAheadBuffer;
        Utility::Algorithm::toLowerCaseInplace(needle);

        auto& items = impl_->items.value();

        // When a filter is active, non-matching items don't render, so only consider items
        // that are in the filter's match set; otherwise every item is a candidate.
        const bool filterActive = !impl_->filterQuery.value().empty();

        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (!items[i].isSelectable())
                continue;
            if (filterActive && impl_->filterMatchPosition.find(static_cast<long long>(i)) ==
                                    impl_->filterMatchPosition.end())
                continue;

            auto name = items[i].displayFilename;
            Utility::Algorithm::toLowerCaseInplace(name);
            if (name.size() >= needle.size() && name.compare(0, needle.size(), needle) == 0)
            {
                impl_->selectionManager.jumpTo(i);
                Nui::globalEventContext.executeActiveEventsImmediately();
                return;
            }
        }
    }

    void Side::clearTypeAhead()
    {
        impl_->typeAheadBuffer.clear();
    }

    void Side::applyFilter(std::string query)
    {
        if (impl_->filterQuery.value() == query)
            return;

        impl_->filterMatchIndices.clear();
        impl_->filterMatchPosition.clear();

        if (!query.empty())
        {
            Utility::Algorithm::toLowerCaseInplace(query);
            const auto queryTokens = tokenize(query);
            auto const& items = impl_->items.value();
            for (std::size_t itemIdx = 0; itemIdx < items.size(); ++itemIdx)
            {
                const auto generic = items[itemIdx].item.path.generic_string();
                if (generic.find(query) != std::string::npos ||
                    scoreItemAgainstQuery(tokenize(generic), queryTokens) >= kSearchMinimumScore)
                {
                    const auto absoluteIndex = static_cast<long long>(itemIdx);
                    impl_->filterMatchPosition.emplace(
                        absoluteIndex, static_cast<long long>(impl_->filterMatchIndices.size())
                    );
                    impl_->filterMatchIndices.push_back(absoluteIndex);
                }
            }
        }

        // Pagination must apply to the filtered set, not the underlying vector. When the filter
        // is empty the regular pagination (computed off items.size()) takes over again.
        const auto pageSizeValue = std::max(1, impl_->pageSize.value());
        const std::size_t paginationCount = query.empty() ? impl_->items.value().size()
                                                          : impl_->filterMatchIndices.size();
        const int newPageCount = std::max(
            1,
            static_cast<int>(
                (paginationCount + static_cast<std::size_t>(pageSizeValue) - 1) /
                static_cast<std::size_t>(pageSizeValue)
            )
        );
        if (impl_->pageCount.value() != newPageCount)
            impl_->pageCount = newPageCount;
        if (impl_->currentPage.value() != 0)
            impl_->currentPage = 0;

        impl_->filterQuery = std::move(query);
        impl_->items.modifyNow();
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
}