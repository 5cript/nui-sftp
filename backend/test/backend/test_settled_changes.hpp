#pragma once

#include <backend/file_tracking/settled_changes.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace Test
{
    class SettledChangesTest : public ::testing::Test
    {
      protected:
        boost::asio::io_context ioc_;
        boost::asio::strand<boost::asio::any_io_executor> strand_{ioc_.get_executor()};
        std::vector<FileTracking::ReportedChange> sent_;
        std::shared_ptr<FileTracking::SettledChanges> changes_ = std::make_shared<FileTracking::SettledChanges>(
            strand_,
            [this](FileTracking::ReportedChange const& change)
            {
                sent_.push_back(change);
            },
            std::chrono::milliseconds{20}
        );

        void runFor(std::chrono::milliseconds duration)
        {
            ioc_.restart();
            ioc_.run_for(duration);
        }

        void add(
            FileTracking::FileAction action,
            std::filesystem::path const& path,
            bool isFileContent,
            std::filesystem::path const& oldPath = {}
        )
        {
            changes_->add(
                FileTracking::ReportedChange{.action = action, .path = path, .oldPath = oldPath}, isFileContent
            );
        }
    };

    using FileTracking::FileAction;

    TEST_F(SettledChangesTest, ContentChangesOfOneFileInARowAreSentOnce)
    {
        add(FileAction::Modified, "/watched/notes.txt", true);
        add(FileAction::Modified, "/watched/notes.txt", true);
        EXPECT_TRUE(sent_.empty());

        runFor(std::chrono::milliseconds{200});

        ASSERT_EQ(sent_.size(), 1u);
        EXPECT_EQ(sent_[0].path, "/watched/notes.txt");
        EXPECT_EQ(sent_[0].action, FileAction::Modified);
    }

    TEST_F(SettledChangesTest, AnAdditionStaysAnAddition)
    {
        add(FileAction::Added, "/watched/new.txt", true);
        add(FileAction::Modified, "/watched/new.txt", true);

        runFor(std::chrono::milliseconds{200});

        ASSERT_EQ(sent_.size(), 1u);
        EXPECT_EQ(sent_[0].action, FileAction::Added);
    }

    TEST_F(SettledChangesTest, ARemovalLeavesTheHeldBackChangesNextToItAlone)
    {
        add(FileAction::Modified, "/watched/tree-copy/file.txt", true);
        add(FileAction::Modified, "/watched/tree.txt", true);
        add(FileAction::Modified, "/watched/tre/file.txt", true);
        add(FileAction::Modified, "/watched/tree/file.txt", true);
        add(FileAction::Deleted, "/watched/./tree", false);

        ASSERT_EQ(sent_.size(), 2u);
        EXPECT_EQ(sent_[0].path, "/watched/tree/file.txt");
        EXPECT_EQ(sent_[1].path, "/watched/./tree");

        runFor(std::chrono::milliseconds{200});

        EXPECT_EQ(sent_.size(), 5u);
    }

    TEST_F(SettledChangesTest, ContentChangesOfDifferentFilesAreSentEach)
    {
        add(FileAction::Modified, "/watched/one.txt", true);
        add(FileAction::Modified, "/watched/two.txt", true);

        runFor(std::chrono::milliseconds{200});

        EXPECT_EQ(sent_.size(), 2u);
    }

    TEST_F(SettledChangesTest, ARemovalSendsTheHeldBackChangesAtAndBelowItFirst)
    {
        add(FileAction::Added, "/watched/tree/file.txt", true);
        add(FileAction::Modified, "/watched/kept.txt", true);
        add(FileAction::Modified, "/watched/tree/later.txt", true);
        add(FileAction::Deleted, "/watched/tree", false);

        ASSERT_EQ(sent_.size(), 3u);
        EXPECT_EQ(sent_[0].action, FileAction::Added);
        EXPECT_EQ(sent_[1].action, FileAction::Modified);
        EXPECT_EQ(sent_[2].action, FileAction::Deleted);
        runFor(std::chrono::milliseconds{200});

        EXPECT_EQ(sent_.size(), 4u);
    }

    TEST_F(SettledChangesTest, AMoveSendsTheHeldBackChangeOfWhereItCameFromFirst)
    {
        add(FileAction::Modified, "/watched/old.txt", true);
        add(FileAction::Moved, "/watched/new.txt", false, "/watched/old.txt");

        ASSERT_EQ(sent_.size(), 2u);
        EXPECT_EQ(sent_[0].action, FileAction::Modified);
        EXPECT_EQ(sent_[1].action, FileAction::Moved);
    }

    TEST_F(SettledChangesTest, AChangeOfADirectorySendsTheHeldBackChangesBelowIt)
    {
        add(FileAction::Modified, "/watched/tree/file.txt", true);
        add(FileAction::Added, "/watched/tree", false);

        ASSERT_EQ(sent_.size(), 2u);
        EXPECT_EQ(sent_[0].action, FileAction::Modified);
        EXPECT_EQ(sent_[1].action, FileAction::Added);
    }

    TEST_F(SettledChangesTest, AnotherClassOfChangeAtTheSameFileSendsTheHeldBackOneFirst)
    {
        add(FileAction::Modified, "/watched/notes.txt", true);
        add(FileAction::Deleted, "/watched/notes.txt", false);

        ASSERT_EQ(sent_.size(), 2u);
        EXPECT_EQ(sent_[0].action, FileAction::Modified);
        EXPECT_EQ(sent_[1].action, FileAction::Deleted);
        runFor(std::chrono::milliseconds{200});
        EXPECT_EQ(sent_.size(), 2u);
    }

    TEST_F(SettledChangesTest, NothingIsSentOnceItIsGone)
    {
        add(FileAction::Modified, "/watched/notes.txt", true);
        changes_.reset();

        runFor(std::chrono::milliseconds{200});

        EXPECT_TRUE(sent_.empty());
    }
}
