#pragma once

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/icon_size.hpp>
#include <nui-file-explorer/side/side_settings.hpp>
#include <nui-file-explorer/side/selection_manager.hpp>

#include <nui/frontend/val.hpp>

#include <script-nui-components/popup_menu.hpp>
#include <script-nui-components/dropdown_menu.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/event_system/listen.hpp>

#include <utility/enum_string_convert.hpp>
#include <utility/format_bytes.hpp>

#include <unordered_map>
#include <vector>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    class Side;

    enum class SortCriterion
    {
        Name,
        Size,
        Info,
        Mtime
    };

    struct SideImplementation
    {
        SideSettings settings;
        Side* otherSide{nullptr};
        std::unique_ptr<ISideModel> model;

        Nui::Observed<std::vector<ItemWithInternals>> items{};
        Nui::Observed<Flavor> flavor{Flavor::Icons};
        SelectionManager selectionManager{items, flavor.value()};
        Nui::Observed<unsigned int> iconSize{static_cast<unsigned int>(IconSize::Medium)};
        Nui::Observed<unsigned int> iconSpacing{48u};
        Nui::Observed<std::pair<SortCriterion, bool>> sorting{{SortCriterion::Name, true}};
        Nui::Observed<bool> showHiddenFiles{false};

        std::weak_ptr<Nui::Dom::BasicElement> sideElement{};
        std::weak_ptr<Nui::Dom::BasicElement> scrollContainer{};
        std::weak_ptr<Nui::Dom::BasicElement> searchTextBox{};

        Nui::Observed<bool> isPlacesWide{false};
        Nui::Observed<std::optional<bool>> isPlacesOpen{std::nullopt};
        Nui::val resizeObserver{Nui::val::undefined()};
        // Lifetime sentinel for ResizeObserver callback: the browser can deliver a queued
        // entry after ~Side, at which point `this` is dangling. The callback locks this
        // weak_ptr and no-ops when the Side is gone.
        std::shared_ptr<bool> alive = std::make_shared<bool>(true);

        // Loading-state plumbing for the side. `isLoading` flips immediately when navigation
        // starts; `showLoadingHint` only flips after a debounce so fast navigations don't flash
        // a placeholder element. `loadingHintTimerHandle` carries the setTimeout id to cancel.
        Nui::Observed<bool> isLoading{false};
        Nui::Observed<bool> showLoadingHint{false};
        Nui::val loadingHintTimerHandle{Nui::val::undefined()};
        Nui::ListenRemover<Nui::Observed<std::filesystem::path>> currentPathListener{};

        // Pagination — render-only slicing. SelectionManager remains index-absolute against
        // the full items vector. Default pageSize is intentionally large (the user prefers
        // unpaginated UX); the footer hides itself when pageCount <= 1.
        Nui::Observed<int> currentPage{0};
        Nui::Observed<int> pageSize{500};
        Nui::Observed<int> pageCount{1};

        // Active search filter (paginated directories). Empty means "no filter". The live
        // highlight mode used for small directories sets per-item `searchHighlight` instead.
        // `filterMatchIndices` is the absolute item indices that pass the filter, in order.
        // `filterMatchPosition` maps an absolute index to its position within the filtered
        // sequence, so the flavor render lambda can decide if the match falls on the current
        // pagination page in O(1).
        Nui::Observed<std::string> filterQuery{""};
        std::vector<long long> filterMatchIndices{};
        std::unordered_map<long long, long long> filterMatchPosition{};

        Nui::Observed<std::vector<std::filesystem::path>> pathBoxSuggestions{};
        std::map<long long, std::weak_ptr<Nui::Dom::BasicElement>> searchResultElements;
        std::weak_ptr<Nui::Dom::BasicElement> pathBoxElement{};

        std::vector<Item> copiedFiles{};
        std::function<void(std::filesystem::path, std::filesystem::path)> onSynchronize{};

        ScriptNuiComponents::DropdownMenu newItemMenu{};
        ScriptNuiComponents::DropdownMenu sortMenu{};
        ScriptNuiComponents::DropdownMenu viewMenu{};
        ScriptNuiComponents::PopupMenu contextMenuPopup{};

        void sortItems()
        {
            const auto [criterion, ascending] = sorting.value();
            switch (criterion)
            {
                case SortCriterion::Name:
                    sortByName(ascending);
                    break;
                case SortCriterion::Size:
                    sortBySize(ascending);
                    break;
                case SortCriterion::Info:
                    sortByInfo(ascending);
                    break;
                case SortCriterion::Mtime:
                    sortByMtime(ascending);
                    break;
            }
        }

        auto partitionItems()
        {
            auto& items = this->items.value();
            return std::stable_partition(
                items.begin(),
                items.end(),
                [](auto const& lhs)
                {
                    return lhs.item.isDirectoryLike();
                }
            );
        }

        void sortByPredicate(auto const& predicate)
        {
            auto partitionBorder = partitionItems();
            auto& items = this->items.value();
            std::sort(items.begin(), partitionBorder, predicate);
            if (partitionBorder != items.end())
                std::sort(partitionBorder, items.end(), predicate);
        }

        void sortByName(bool ascending)
        {
            if (ascending)
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.path.filename() < rhs.item.path.filename();
                    }
                );
            }
            else
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.path.filename() > rhs.item.path.filename();
                    }
                );
            }
        }
        void sortByInfo(bool ascending)
        {
            if (ascending)
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.lsStyleTypePermsUserGroup() < rhs.item.lsStyleTypePermsUserGroup();
                    }
                );
            }
            else
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.lsStyleTypePermsUserGroup() > rhs.item.lsStyleTypePermsUserGroup();
                    }
                );
            }
        }
        void sortBySize(bool ascending)
        {
            if (ascending)
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.size < rhs.item.size;
                    }
                );
            }
            else
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.size > rhs.item.size;
                    }
                );
            }
        }
        void sortByMtime(bool ascending)
        {
            if (ascending)
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.mtime < rhs.item.mtime;
                    }
                );
            }
            else
            {
                sortByPredicate(
                    [](auto const& lhs, auto const& rhs)
                    {
                        return lhs.item.mtime > rhs.item.mtime;
                    }
                );
            }
        }

        SideImplementation(SideSettings settings, std::unique_ptr<ISideModel> model)
            : settings{std::move(settings)}
            , model{std::move(model)}
            , showHiddenFiles{this->settings.showHiddenFiles}
            , pageSize{std::max(1, this->settings.pageSize)}
        {
            namespace Snc = ScriptNuiComponents;

            newItemMenu.setOnOpen(
                [this]()
                {
                    sortMenu.close();
                    viewMenu.close();
                    contextMenuPopup.close();
                }
            );
            sortMenu.setOnOpen(
                [this]()
                {
                    newItemMenu.close();
                    viewMenu.close();
                    contextMenuPopup.close();
                }
            );
            viewMenu.setOnOpen(
                [this]()
                {
                    newItemMenu.close();
                    sortMenu.close();
                    contextMenuPopup.close();
                }
            );

            newItemMenu.setItems({
                Snc::PopupMenu::item(
                    "File",
                    std::string{},
                    [this]()
                    {
                        this->model->onNewItem(Item::Type::Regular);
                    }
                ),
                Snc::PopupMenu::item(
                    "Folder",
                    std::string{},
                    [this]()
                    {
                        this->model->onNewItem(Item::Type::Directory);
                    }
                ),
            });

            sortMenu.setItems({
                Snc::PopupMenu::item(
                    "Name Ascending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Name, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Name Descending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Name, false};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Size Ascending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Size, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Size Descending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Size, false};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Info Ascending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Info, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Info Descending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Info, false};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Modification Time Ascending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Mtime, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Modification Time Descending",
                    std::string{},
                    [this]()
                    {
                        sorting = {SortCriterion::Mtime, false};
                        sortItems();
                        items.modifyNow();
                    }
                ),
            });

            viewMenu.setItems({
                Snc::PopupMenu::item(
                    "Icons",
                    std::string{},
                    [this]()
                    {
                        flavor = Flavor::Icons;
                        selectionManager.setFlavor(flavor.value());
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }
                ),
                Snc::PopupMenu::item(
                    "Table",
                    std::string{},
                    [this]()
                    {
                        flavor = Flavor::Table;
                        selectionManager.setFlavor(flavor.value());
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }
                ),
            });

            selectionManager.setScrollIntoViewCallback(
                [this](std::size_t idx)
                {
                    // Keyboard navigation can land us on an item that's outside the current
                    // pagination slice — in that case we first jump to the page that contains
                    // the active index, force the render flush so the element materializes,
                    // and only then scroll it into view.
                    const auto pageSizeValue = std::max(1, pageSize.value());
                    const int targetPage = static_cast<int>(idx / static_cast<std::size_t>(pageSizeValue));
                    if (targetPage != currentPage.value())
                    {
                        currentPage = targetPage;
                        items.modifyNow();
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }

                    auto elem = items.value()[idx].element.lock();
                    if (elem)
                    {
                        auto options = Nui::val::object();
                        options.set("behavior", "auto"s);
                        options.set("block", "nearest"s);
                        options.set("inline", "nearest"s);
                        options.set("container", "nearest"s);
                        options.set("scrollMode", "if-needed"s);
                        elem->val().call<void>("scrollIntoView", options);
                    }
                }
            );
        }
    };
}
