#pragma once

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/item_with_internals.hpp>
#include <nui-file-explorer/dropdown_menu.hpp>
#include <nui-file-explorer/icon_size.hpp>
#include <nui-file-explorer/side/side_settings.hpp>
#include <nui-file-explorer/side/selection_manager.hpp>

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
        constexpr static std::array<std::string_view, 4> tableGridTemplateColumnsDefaults =
            {"1fr", "max-content", "max-content", "max-content"};
        constexpr static std::string_view tableGridTemplateColumnsHiddenValue = "25px";

        SideSettings settings;
        Side* otherSide{nullptr};
        std::unique_ptr<ISideModel> model;

        Nui::Observed<std::vector<ItemWithInternals>> items{};
        Nui::Observed<Flavor> flavor{Flavor::Icons};
        SelectionManager selectionManager{items, flavor.value()};
        Nui::Observed<unsigned int> iconSize{static_cast<unsigned int>(IconSize::Medium)};
        Nui::Observed<unsigned int> iconSpacing{48u};
        Nui::Observed<std::pair<SortCriterion, bool>> sorting{{SortCriterion::Name, true}};
        Nui::Observed<std::vector<std::string>> tableGridTemplateColumns{
            std::vector<std::string>(tableGridTemplateColumnsDefaults.begin(), tableGridTemplateColumnsDefaults.end())
        };
        Nui::Observed<bool> showHiddenFiles{false};

        std::weak_ptr<Nui::Dom::BasicElement> sideElement{};
        std::weak_ptr<Nui::Dom::BasicElement> scrollContainer{};
        std::weak_ptr<Nui::Dom::BasicElement> searchTextBox{};

        Nui::Observed<std::vector<std::filesystem::path>> pathBoxSuggestions{};
        std::map<long long, std::weak_ptr<Nui::Dom::BasicElement>> searchResultElements;
        std::weak_ptr<Nui::Dom::BasicElement> pathBoxElement{};

        std::vector<Item> copiedFiles{};

        DropdownMenu newItemMenu{
            {
                "File",
                "Folder",
                // Soft Link ?
                // Hard Link ?
            },
            [this](std::string const& item)
            {
                Nui::WebApi::Console::log("New clicked: ", item);
                if (item == "File")
                {
                    model->onNewItem(Item::Type::Regular);
                }
                else if (item == "Folder")
                {
                    model->onNewItem(Item::Type::Directory);
                }
            },
            [this]()
            {
                sortMenu.close();
                viewMenu.close();
            },
            "New",
        };
        DropdownMenu sortMenu{
            {
                "Name Ascending",
                "Name Descending",
                "Size Ascending",
                "Size Descending",
                "Info Ascending",
                "Info Descending",
                "Modification Time Ascending",
                "Modification Time Descending",
            },
            [this](std::string const& item)
            {
                if (item == "Name Ascending")
                    sorting = {SortCriterion::Name, true};
                else if (item == "Name Descending")
                    sorting = {SortCriterion::Name, false};
                else if (item == "Size Ascending")
                    sorting = {SortCriterion::Size, true};
                else if (item == "Size Descending")
                    sorting = {SortCriterion::Size, false};
                else if (item == "Info Ascending")
                    sorting = {SortCriterion::Info, true};
                else if (item == "Info Descending")
                    sorting = {SortCriterion::Info, false};
                else if (item == "Modification Time Ascending")
                    sorting = {SortCriterion::Mtime, true};
                else if (item == "Modification Time Descending")
                    sorting = {SortCriterion::Mtime, false};

                sortItems();
                items.modifyNow();
            },
            [this]()
            {
                newItemMenu.close();
                viewMenu.close();
            },
            "Sort",
        };
        DropdownMenu viewMenu{
            {"Icons", "Table"},
            [this](std::string const& item)
            {
                if (item == "Icons")
                    flavor = Flavor::Icons;
                if (item == "Table")
                    flavor = Flavor::Table;
                selectionManager.setFlavor(flavor.value());
                Nui::globalEventContext.executeActiveEventsImmediately();
            },
            [this]()
            {
                newItemMenu.close();
                sortMenu.close();
            },
            "View",
        };

        std::weak_ptr<Nui::Dom::BasicElement> contextMenuView{};
        std::vector<Item> contextMenuClickItems{};

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
            selectionManager.setScrollIntoViewCallback(
                [this](std::size_t idx)
                {
                    using namespace std::string_literals;

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