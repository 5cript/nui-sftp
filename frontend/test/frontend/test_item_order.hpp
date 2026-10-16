#pragma once

#include <nui-file-explorer/side/item_order.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace NuiFileExplorer::Test
{
    namespace ItemOrderTestDetail
    {
        inline Item itemOf(std::string name, Item::Type type, std::uint64_t size = 0u)
        {
            return Item{SharedData::DirectoryEntry{.name = std::move(name), .type = type, .size = size}};
        }

        inline Item linkToDirectory(std::string name)
        {
            auto entry = SharedData::DirectoryEntry{.name = std::move(name), .type = Item::Type::Symlink};
            entry.resolvedTarget = std::make_shared<SharedData::DirectoryEntry>(
                SharedData::DirectoryEntry{.name = "target", .type = Item::Type::Directory}
            );
            return Item{std::move(entry)};
        }

        inline std::vector<std::string> sortedNames(std::vector<Item> items, SortCriterion criterion, bool ascending)
        {
            sortItems(
                items,
                criterion,
                ascending,
                [](Item const& item) -> Item const&
                {
                    return item;
                }
            );
            std::vector<std::string> names;
            std::ranges::transform(
                items,
                std::back_inserter(names),
                [](Item const& item)
                {
                    return std::string{item.displayName()};
                }
            );
            return names;
        }
    }

    TEST(ItemOrderTests, TheParentLinkLeadsInAscendingAndDescendingOrder)
    {
        using namespace ItemOrderTestDetail;
        const std::vector<Item> items{
            itemOf("b", Item::Type::Directory),
            itemOf("a", Item::Type::Regular),
            Item::parentLink("/home"),
            itemOf("c", Item::Type::Directory),
        };
        EXPECT_EQ(sortedNames(items, SortCriterion::Name, true), (std::vector<std::string>{"..", "b", "c", "a"}));
        EXPECT_EQ(sortedNames(items, SortCriterion::Name, false), (std::vector<std::string>{"..", "c", "b", "a"}));
    }

    TEST(ItemOrderTests, DirectoriesAndLinksToThemComeBeforeFiles)
    {
        using namespace ItemOrderTestDetail;
        const std::vector<Item> items{
            itemOf("a", Item::Type::Regular),
            linkToDirectory("link"),
            itemOf("z", Item::Type::Directory),
            itemOf("b", Item::Type::Symlink),
        };
        EXPECT_EQ(
            sortedNames(items, SortCriterion::Name, true), (std::vector<std::string>{"link", "z", "a", "b"})
        );
    }

    TEST(ItemOrderTests, NamesCompareIgnoringCase)
    {
        using namespace ItemOrderTestDetail;
        const std::vector<Item> items{
            itemOf("beta", Item::Type::Regular),
            itemOf("Alpha", Item::Type::Regular),
            itemOf("alphabet", Item::Type::Regular),
            itemOf("Gamma", Item::Type::Regular),
        };
        EXPECT_EQ(
            sortedNames(items, SortCriterion::Name, true),
            (std::vector<std::string>{"Alpha", "alphabet", "beta", "Gamma"})
        );
    }

    TEST(ItemOrderTests, SizesOrderWithinEachGroup)
    {
        using namespace ItemOrderTestDetail;
        const std::vector<Item> items{
            itemOf("big", Item::Type::Regular, 300u),
            itemOf("dir", Item::Type::Directory, 4096u),
            itemOf("small", Item::Type::Regular, 10u),
            Item::parentLink("/"),
        };
        EXPECT_EQ(
            sortedNames(items, SortCriterion::Size, true), (std::vector<std::string>{"..", "dir", "small", "big"})
        );
        EXPECT_EQ(
            sortedNames(items, SortCriterion::Size, false), (std::vector<std::string>{"..", "dir", "big", "small"})
        );
    }

    TEST(ItemOrderTests, AListingWithoutParentLinkOrFilesSorts)
    {
        using namespace ItemOrderTestDetail;
        EXPECT_TRUE(sortedNames({}, SortCriterion::Name, true).empty());
        EXPECT_EQ(
            sortedNames({itemOf("b", Item::Type::Directory), itemOf("a", Item::Type::Directory)}, SortCriterion::Name, true),
            (std::vector<std::string>{"a", "b"})
        );
    }
}
