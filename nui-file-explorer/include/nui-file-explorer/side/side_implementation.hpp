#pragma once

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/icon_size.hpp>
#include <nui-file-explorer/side/side_settings.hpp>
#include <nui-file-explorer/side/selection_manager.hpp>

#include <script-nui-components/popup_menu.hpp>
#include <script-nui-components/dropdown_menu.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/event_system/listen.hpp>

#include <utility/enum_string_convert.hpp>
#include <utility/format_bytes.hpp>

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

        Nui::Observed<std::vector<std::filesystem::path>> pathBoxSuggestions{};
        std::map<long long, std::weak_ptr<Nui::Dom::BasicElement>> searchResultElements;
        std::weak_ptr<Nui::Dom::BasicElement> pathBoxElement{};

        std::vector<Item> copiedFiles{};

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
        {
            namespace Snc = ScriptNuiComponents;

            newItemMenu.setOnOpen([this]() { sortMenu.close(); viewMenu.close(); contextMenuPopup.close(); });
            sortMenu.setOnOpen([this]() { newItemMenu.close(); viewMenu.close(); contextMenuPopup.close(); });
            viewMenu.setOnOpen([this]() { newItemMenu.close(); sortMenu.close(); contextMenuPopup.close(); });

            newItemMenu.setItems({
                Snc::PopupMenu::item(
                    "File",
                    {},
                    [this]()
                    {
                        this->model->onNewItem(Item::Type::Regular);
                    }
                ),
                Snc::PopupMenu::item(
                    "Folder",
                    {},
                    [this]()
                    {
                        this->model->onNewItem(Item::Type::Directory);
                    }
                ),
            });

            sortMenu.setItems({
                Snc::PopupMenu::item(
                    "Name Ascending",
                    {},
                    [this]()
                    {
                        sorting = {SortCriterion::Name, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Name Descending",
                    {},
                    [this]()
                    {
                        sorting = {SortCriterion::Name, false};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Size Ascending",
                    {},
                    [this]()
                    {
                        sorting = {SortCriterion::Size, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Size Descending",
                    {},
                    [this]()
                    {
                        sorting = {SortCriterion::Size, false};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Info Ascending",
                    {},
                    [this]()
                    {
                        sorting = {SortCriterion::Info, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Info Descending",
                    {},
                    [this]()
                    {
                        sorting = {SortCriterion::Info, false};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Modification Time Ascending",
                    {},
                    [this]()
                    {
                        sorting = {SortCriterion::Mtime, true};
                        sortItems();
                        items.modifyNow();
                    }
                ),
                Snc::PopupMenu::item(
                    "Modification Time Descending",
                    {},
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
                    {},
                    [this]()
                    {
                        flavor = Flavor::Icons;
                        selectionManager.setFlavor(flavor.value());
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }
                ),
                Snc::PopupMenu::item(
                    "Table",
                    {},
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
