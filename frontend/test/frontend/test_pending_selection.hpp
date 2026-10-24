#pragma once

#include <nui-file-explorer/side/pending_selection.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

namespace NuiFileExplorer::Test
{
    TEST(PendingSelectionTests, NothingIsPendingAtFirst)
    {
        PendingSelection selection{};

        EXPECT_FALSE(selection.isPending());
        EXPECT_EQ(selection.take("/home"), std::nullopt);
    }

    TEST(PendingSelectionTests, TheListingOfTheDirectoryYieldsTheItemPath)
    {
        PendingSelection selection{};
        selection.request("/home/user", "notes.txt");

        EXPECT_TRUE(selection.isPending());
        EXPECT_EQ(selection.take("/home/user"), std::filesystem::path{"/home/user/notes.txt"});
        EXPECT_FALSE(selection.isPending());
    }

    TEST(PendingSelectionTests, AnItemOfTheRootJoinsWithoutASecondSeparator)
    {
        PendingSelection selection{};
        selection.request("/", "etc");

        EXPECT_EQ(selection.take("/"), std::filesystem::path{"/etc"});
    }

    TEST(PendingSelectionTests, TheRequestIsTakenOnlyOnce)
    {
        PendingSelection selection{};
        selection.request("/home/user", "notes.txt");
        selection.take("/home/user");

        EXPECT_EQ(selection.take("/home/user"), std::nullopt);
    }

    TEST(PendingSelectionTests, TheListingOfAnotherDirectoryEndsTheRequest)
    {
        PendingSelection selection{};
        selection.request("/home/user", "notes.txt");

        EXPECT_EQ(selection.take("/home"), std::nullopt);
        EXPECT_FALSE(selection.isPending());
        EXPECT_EQ(selection.take("/home/user"), std::nullopt);
    }

    TEST(PendingSelectionTests, MovingToTheDirectoryKeepsTheRequest)
    {
        PendingSelection selection{};
        selection.request("/home/user", "notes.txt");
        selection.onPathChanged("/home/user");

        EXPECT_EQ(selection.take("/home/user"), std::filesystem::path{"/home/user/notes.txt"});
    }

    TEST(PendingSelectionTests, MovingElsewhereCancelsTheRequest)
    {
        PendingSelection selection{};
        selection.request("/home/user", "notes.txt");
        selection.onPathChanged("/tmp");
        selection.onPathChanged("/home/user");

        EXPECT_FALSE(selection.isPending());
        EXPECT_EQ(selection.take("/home/user"), std::nullopt);
    }

    TEST(PendingSelectionTests, ALaterRequestReplacesTheEarlierOne)
    {
        PendingSelection selection{};
        selection.request("/home/user", "notes.txt");
        selection.request("/srv", "data");

        EXPECT_EQ(selection.take("/srv"), std::filesystem::path{"/srv/data"});
    }
}
