#pragma once

#include <nui-file-explorer/transfer_target.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

namespace NuiFileExplorer::Test
{
    namespace TransferTargetTestDetail
    {
        inline Item itemOf(std::string name, Item::Type type)
        {
            return Item{SharedData::DirectoryEntry{.name = std::move(name), .type = type}};
        }
    }

    TEST(TransferTargetTests, TheParentEntryTakesItemsIntoTheParent)
    {
        const auto target = transferTargetOf(Item::parentLink("/home"));
        ASSERT_TRUE(target);
        EXPECT_EQ(*target, std::optional<std::string>{".."});
    }

    TEST(TransferTargetTests, ADirectoryTakesItemsByItsName)
    {
        using namespace TransferTargetTestDetail;
        const auto target = transferTargetOf(itemOf("docs", Item::Type::Directory));
        ASSERT_TRUE(target);
        EXPECT_EQ(*target, std::optional<std::string>{"docs"});
    }

    TEST(TransferTargetTests, ALinkToADirectoryTakesItemsByItsName)
    {
        using namespace TransferTargetTestDetail;
        auto link = itemOf("linkToDocs", Item::Type::Symlink);
        link.resolvedTarget =
            std::make_shared<SharedData::DirectoryEntry>(SharedData::DirectoryEntry{.type = Item::Type::Directory});
        const auto target = transferTargetOf(link);
        ASSERT_TRUE(target);
        EXPECT_EQ(*target, std::optional<std::string>{"linkToDocs"});
    }

    TEST(TransferTargetTests, OtherItemsLeaveItemsInTheShownDirectory)
    {
        using namespace TransferTargetTestDetail;
        auto fileLink = itemOf("linkToFile", Item::Type::Symlink);
        fileLink.resolvedTarget =
            std::make_shared<SharedData::DirectoryEntry>(SharedData::DirectoryEntry{.type = Item::Type::Regular});
        for (auto const& item : {itemOf("file.txt", Item::Type::Regular), itemOf("dangling", Item::Type::Symlink), fileLink})
        {
            const auto target = transferTargetOf(item);
            ASSERT_TRUE(target) << item.name;
            EXPECT_EQ(*target, std::nullopt) << item.name;
        }
    }

    TEST(TransferTargetTests, ADirectoryWithAnInvalidNameIsNoTarget)
    {
        using namespace TransferTargetTestDetail;
        auto directory = itemOf("bad\xEF\xBF\xBDname", Item::Type::Directory);
        directory.nameIsValidUtf8 = false;
        const auto target = transferTargetOf(directory);
        ASSERT_FALSE(target);
        EXPECT_EQ(target.error().code, ErrorCode::DropTargetNameInvalid);
        EXPECT_EQ(target.error().detail, directory.name);
    }
}
