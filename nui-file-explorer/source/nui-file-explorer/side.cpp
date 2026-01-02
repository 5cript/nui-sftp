#include <nui-file-explorer/side.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/frontend/api/drag_event.hpp>

#include <utility/enum_string_convert.hpp>
#include <utility/format_bytes.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <rapidfuzz/fuzz.hpp>

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
                    impl_->scrollContainer_ = ref;
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

        auto selectedPaths = impl_->selectionManager.selectedPaths();
        impl_->selectionManager.loseTrackToAllSelections();

        std::set<std::filesystem::path> pathSet;

        impl_->items.value().clear();
        std::transform(
            items.begin(),
            items.end(),
            std::back_inserter(impl_->items.value()),
            [&pathSet](auto const& item)
            {
                pathSet.insert(item.path);
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
        if (auto searchTextBox = impl_->searchTextBox_.lock(); searchTextBox)
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
        closeMenus();
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

    void Side::closeMenus()
    {
        impl_->newItemMenu.close();
        impl_->sortMenu.close();
        impl_->viewMenu.close();

        if (const auto menu = impl_->contextMenuView.lock(); menu)
            menu->val()["style"].set("display", "none");
    }

    void Side::onContextMenu(ItemWithInternals* item, Nui::val event)
    {
        using namespace std::string_literals;

        event.call<void>("stopPropagation");
        event.call<void>("preventDefault");
        if (const auto menu = impl_->contextMenuView.lock(); menu)
        {
            const auto offsetParent = event["target"]["offsetParent"];

            int targetOffsetTop = event["target"]["offsetTop"].as<int>();
            int targetOffsetLeft = event["target"]["offsetLeft"].as<int>();
            if (!offsetParent.isUndefined() && !offsetParent.isNull())
            {
                for (auto const& cls : offsetParent["classList"])
                {
                    if (cls.as<std::string>() == "nui-file-grid-table-cell" ||
                        cls.as<std::string>() == "nui-file-grid-item-icons")
                    {
                        targetOffsetTop = offsetParent["offsetTop"].as<int>() + targetOffsetTop;
                        targetOffsetLeft = offsetParent["offsetLeft"].as<int>() + targetOffsetLeft;
                        break;
                    }
                }
            }
            const int offsetY = event["offsetY"].as<int>();
            const int offsetX = event["offsetX"].as<int>();

            const auto left = targetOffsetLeft + offsetX;
            const auto top = targetOffsetTop + offsetY;

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

            menu->val()["style"].set("display", "block");
            menu->val()["style"].set("top", std::to_string(top) + "px");
            menu->val()["style"].set("left", std::to_string(left) + "px");
            return;
        }
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

        // clang-format off
        return div {
            class_ = "nui-file-grid-head"
        }(
            impl_->newItemMenu(),
            impl_->sortMenu(),
            impl_->viewMenu(),
            filter()
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
                type = "text",
                "value"_prop = observe(model().currentPath()).generate([this](){
                    return model().currentPath().value().generic_string();
                }),
                "keyup"_event = [this](Nui::val event){
                    if (event["key"].as<std::string>() == "Enter")
                        impl_->model->onPathChange(std::filesystem::path{event["target"]["value"].as<std::string>()});
                    event.call<void>("stopPropagation");
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
                    impl_->searchTextBox_ = ref;
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
                item.searchHighlighted = ItemWithInternals::SearchHighlight::Off;
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
                    auto const fuzzScore = rapidfuzz::fuzz::ratio(query, *i);
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
                item.searchHighlighted = ItemWithInternals::SearchHighlight::Highlight;
                continue;
            }
            const auto reachedScore = score(tokenize(generic));
            item.searchHighlighted = reachedScore.first >= minimumScore ? ItemWithInternals::SearchHighlight::Highlight
                                                                        : ItemWithInternals::SearchHighlight::Muted;
        }
    }
}