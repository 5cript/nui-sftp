#pragma once

#include <backend/dropped_entry.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    TEST(DroppedEntryTests, DescribesWhatLiesAtThePathItself)
    {
        const Utility::TemporaryDirectory directory{temporaryRoot, true};
        std::filesystem::create_directories(directory.path() / "folder");
        std::ofstream{directory.path() / "file.txt"} << "x";

        const auto file = droppedItemFromPath(directory.path() / "file.txt");
        ASSERT_TRUE(file);
        EXPECT_EQ(file->entry.type, SharedData::FileType::Regular);
        EXPECT_EQ(file->entry.name, "file.txt");
        const auto folder = droppedItemFromPath(directory.path() / "folder");
        ASSERT_TRUE(folder);
        EXPECT_EQ(folder->entry.type, SharedData::FileType::Directory);

        std::error_code linkError{};
        std::filesystem::create_directory_symlink(directory.path() / "folder", directory.path() / "link", linkError);
        if (!linkError)
        {
            const auto link = droppedItemFromPath(directory.path() / "link");
            ASSERT_TRUE(link);
            EXPECT_EQ(link->entry.type, SharedData::FileType::Symlink) << "a dropped link is not followed";
        }
    }

    TEST(DroppedEntryTests, RefusesWhatCannotBeDescribed)
    {
        const Utility::TemporaryDirectory directory{temporaryRoot, true};
        EXPECT_FALSE(droppedItemFromPath(directory.path() / "missing"));
        EXPECT_FALSE(droppedItemFromPath("relative/path"));
    }
}
