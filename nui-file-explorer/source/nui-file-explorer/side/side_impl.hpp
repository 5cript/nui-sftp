#include <nui-file-explorer/side.hpp>
#include <nui-file-explorer/dropdown_menu.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/event_system/listen.hpp>

#include <utility/enum_string_convert.hpp>
#include <utility/format_bytes.hpp>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    enum class SortCriterion
    {
        Name,
        Type,
        Size,
        Atime
    };

    inline std::string secondsSinceEpochToReadable(std::uint64_t epochSeconds)
    {
        return Nui::val::global("Date")
            .new_(static_cast<double>(epochSeconds) * 1000)
            .call<std::string>("toLocaleString");
    }

    struct Side::Implementation
    {
        Settings settings;
        std::unique_ptr<ISideModel> model;

        Nui::Observed<std::vector<ItemWithInternals>> items{};
        Nui::Observed<Flavor> flavor{Flavor::Icons};
        Nui::Observed<unsigned int> iconSize{static_cast<unsigned int>(IconSize::Medium)};
        Nui::Observed<unsigned int> iconSpacing{32u};
        Nui::Observed<std::pair<SortCriterion, bool>> sorting{{SortCriterion::Name, true}};

        DropdownMenu newItemMenu{
            {
                "File",
                "Folder",
                // Soft Link ?
                // Hard Link ?
            },
            [this](std::string const& item)
            {
                Nui::Console::log("New clicked: ", item);
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
                "Name",
                // More...
            },
            [this](std::string const& item)
            {
                if (item == "Name")
                {
                    sortItems();
                    this->items.modifyNow();
                }
            },
            [this]()
            {
                newItemMenu.close();
                viewMenu.close();
            },
            "Sort",
        };
        DropdownMenu viewMenu{
            {
                "Icons",
                "Table",
                "Tiles",
            },
            [this](std::string const& item)
            {
                if (item == "Icons")
                    flavor = Flavor::Icons;
                if (item == "Table")
                    flavor = Flavor::Table;
                if (item == "Tiles")
                    flavor = Flavor::Tiles;
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
                case SortCriterion::Type:
                    sortByType(ascending);
                    break;
                case SortCriterion::Size:
                    sortBySize(ascending);
                    break;
                case SortCriterion::Atime:
                    sortByAtime(ascending);
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
                    return lhs.item.type == Item::Type::Directory;
                }
            );
        }

        void sortByPredicate(auto const& predicate)
        {
            auto& items = this->items.value();
            auto partitionBorder = partitionItems();
            std::sort(items.begin(), partitionBorder, predicate);
            std::sort(partitionBorder, items.end(), predicate);
        }

        void sortByName(bool ascending)
        {
            sortByPredicate(
                [ascending](auto const& lhs, auto const& rhs)
                {
                    if (ascending)
                        return lhs.item.path.filename().string() < rhs.item.path.filename().string();
                    return lhs.item.path.filename().string() > rhs.item.path.filename().string();
                }
            );
        }
        void sortByType(bool ascending)
        {
            sortByPredicate(
                [ascending](auto const& lhs, auto const& rhs)
                {
                    if (ascending)
                        return static_cast<int>(lhs.item.type) < static_cast<int>(rhs.item.type);
                    return static_cast<int>(lhs.item.type) > static_cast<int>(rhs.item.type);
                }
            );
        }
        void sortBySize(bool ascending)
        {
            sortByPredicate(
                [ascending](auto const& lhs, auto const& rhs)
                {
                    if (ascending)
                        return lhs.item.size < rhs.item.size;
                    return lhs.item.size > rhs.item.size;
                }
            );
        }
        void sortByAtime(bool ascending)
        {
            sortByPredicate(
                [ascending](auto const& lhs, auto const& rhs)
                {
                    if (ascending)
                        return lhs.item.atime < rhs.item.atime;
                    return lhs.item.atime > rhs.item.atime;
                }
            );
        }

        Implementation(Settings settings, std::unique_ptr<ISideModel> model)
            : settings{std::move(settings)}
            , model{std::move(model)}
        {}
    };
}