#pragma once

#include <backend/file_tracking/change_filter.hpp>
#include <backend/file_tracking/directory_watch_service.hpp>
#include <backend/file_tracking/directory_watcher.hpp>
#include <backend/file_tracking/file_states.hpp>
#include <backend/file_tracking/local_item_watch.hpp>
#include <backend/file_tracking/local_watch_manager.hpp>
#include <persistence/state/sftp_options.hpp>
#include <paths/native.hpp>
#include <utility/localized_message.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace fs = std::filesystem;

    TEST(LocalWatchScopeTests, AChangeOfTheItemOrBelowItConcernsIt)
    {
        const fs::path item{"/home/user/project"};
        EXPECT_TRUE(FileTracking::isChangeOfItem(item, "/home/user/project", {}));
        EXPECT_TRUE(FileTracking::isChangeOfItem(item, "/home/user/project/src/main.cpp", {}));
        EXPECT_TRUE(FileTracking::isChangeOfItem(item, "/home/user/elsewhere", "/home/user/project"));
        EXPECT_TRUE(FileTracking::isChangeOfItem(item, "/home/user/project", "/home/user/.project.swp"));
    }

    TEST(LocalWatchScopeTests, SiblingsAndPrefixesDoNotConcernTheItem)
    {
        const fs::path item{"/home/user/project"};
        EXPECT_FALSE(FileTracking::isChangeOfItem(item, "/home/user/project2", {}));
        EXPECT_FALSE(FileTracking::isChangeOfItem(item, "/home/user/other.txt", "/home/user/other.tmp"));
        EXPECT_FALSE(FileTracking::isChangeOfItem(item, "/home/user", {}));
    }

    TEST(LocalWatchOverlapTests, NestedOrEqualItemsAreRefused)
    {
        const std::vector<FileTracking::LocalWatchManagerDetail::PlacedPath> watched{fs::path{"/home/user/project"}};
        const fs::path downloads{"/var/tmp/nui-sftp"};
        using FileTracking::LocalWatchManagerDetail::refuseOverlap;
        EXPECT_FALSE(refuseOverlap(fs::path{"/home/user/project"}, watched, downloads).has_value());
        EXPECT_FALSE(refuseOverlap(fs::path{"/home/user/project/src/main.cpp"}, watched, downloads).has_value());
        EXPECT_FALSE(refuseOverlap(fs::path{"/home/user"}, watched, downloads).has_value());
        EXPECT_FALSE(refuseOverlap(fs::path{"/var/tmp/nui-sftp/instance/file.txt"}, watched, downloads).has_value());
        EXPECT_FALSE(refuseOverlap(fs::path{"/var"}, watched, downloads).has_value());
    }

    TEST(LocalWatchOverlapTests, TheRefusalNamesWhatItOverlaps)
    {
        using FileTracking::LocalWatchManagerDetail::refuseOverlap;
        const auto nested = refuseOverlap(
            fs::path{"/home/user/project/src"}, {fs::path{"/home/user/project"}}, fs::path{"/var/tmp/nui-sftp"}
        );
        const auto downloads = refuseOverlap(fs::path{"/var/tmp"}, {}, fs::path{"/var/tmp/nui-sftp"});

        ASSERT_FALSE(nested.has_value());
        EXPECT_EQ(
            Utility::parseLocalizedMessage(nested.error()).key, "backend.fileTracking.watchOverlaps"
        );
        EXPECT_EQ(
            Utility::parseLocalizedMessage(nested.error()).arguments,
            (std::vector<std::string>{"/home/user/project/src", "/home/user/project"})
        );
        ASSERT_FALSE(downloads.has_value());
        EXPECT_EQ(
            Utility::parseLocalizedMessage(downloads.error()).key, "backend.fileTracking.watchOverlapsDownloads"
        );
    }

    TEST(LocalWatchOverlapTests, SiblingsAndPrefixesAreAccepted)
    {
        const std::vector<FileTracking::LocalWatchManagerDetail::PlacedPath> watched{fs::path{"/home/user/project"}};
        const fs::path downloads{"/var/tmp/nui-sftp"};
        using FileTracking::LocalWatchManagerDetail::refuseOverlap;
        EXPECT_TRUE(refuseOverlap(fs::path{"/home/user/project2"}, watched, downloads).has_value());
        EXPECT_TRUE(refuseOverlap(fs::path{"/home/user/notes.txt"}, watched, downloads).has_value());
        EXPECT_TRUE(refuseOverlap(fs::path{"/home/user/notes.txt"}, {}, downloads).has_value());
        EXPECT_TRUE(refuseOverlap(fs::path{"/var/tmp/nui-sftp2"}, {}, downloads).has_value());
    }

#if defined(_WIN32) || defined(__APPLE__)
    TEST(LocalWatchOverlapTests, CaseIsIgnoredWhereTheFileSystemFoldsIt)
    {
        EXPECT_TRUE(FileTracking::LocalWatchManagerDetail::overlaps(fs::path{"/Users/Me/Project"}, fs::path{"/users/me/project/src"}));
    }
#endif

    TEST(LocalWatchOverlapTests, TheSameDirectoryUnderAnotherSpellingOverlaps)
    {
        const auto root = fs::canonical(temporaryRoot) /
            ("overlap_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "project" / "src");
        std::error_code linkError;
        fs::create_directory_symlink(root / "project", root / "alias", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create a symbolic link: " << linkError.message();

        EXPECT_TRUE(FileTracking::LocalWatchManagerDetail::overlaps(root / "alias" / "src", root / "project"));
        EXPECT_TRUE(FileTracking::LocalWatchManagerDetail::overlaps(root / "project", root / "alias" / "src"));
        EXPECT_FALSE(FileTracking::LocalWatchManagerDetail::overlaps(root / "alias" / "missing", root / "other"));
        fs::remove_all(root);
    }

    class ItemStatesTest : public ::testing::Test
    {
      protected:
        fs::path root_;

        void SetUp() override
        {
            root_ = fs::canonical(temporaryRoot) /
                ("item_states_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            fs::create_directories(root_ / "tree" / "below");
            std::ofstream{root_ / "tree" / "below" / "file.txt"} << "content";
            std::ofstream{root_ / "sibling.txt"} << "sibling";
        }

        void TearDown() override
        {
            fs::remove_all(root_);
        }

        static std::vector<fs::path> pathsOf(std::vector<FileTracking::FileState> const& states)
        {
            std::vector<fs::path> paths;
            for (auto const& state : states)
                paths.push_back(state.path);
            std::ranges::sort(paths);
            return paths;
        }
    };

    TEST_F(ItemStatesTest, AFileListsOnlyItself)
    {
        const auto states = FileTracking::itemStatesOf(root_ / "sibling.txt");

        ASSERT_EQ(states.size(), 1u);
        EXPECT_EQ(states[0].path, root_ / "sibling.txt");
        EXPECT_EQ(states[0].size, 7u);
        EXPECT_FALSE(states[0].isDirectory);
        EXPECT_GT(states[0].mtime, 0u);
    }

    TEST_F(ItemStatesTest, ADirectoryListsItselfAndItsContentButNoSiblings)
    {
        const auto states = FileTracking::itemStatesOf(root_ / "tree");

        EXPECT_EQ(
            pathsOf(states),
            (std::vector<fs::path>{
                root_ / "tree",
                root_ / "tree" / "below",
                root_ / "tree" / "below" / "file.txt",
            })
        );
    }

    TEST_F(ItemStatesTest, AMissingItemListsNothing)
    {
        EXPECT_TRUE(FileTracking::itemStatesOf(root_ / "missing").empty());
    }

    class LocalWatchManagerTest : public ::testing::Test
    {
      protected:
        boost::asio::io_context ioc_;
        fs::path root_;
        std::unique_ptr<FileTracking::LocalWatchManager> manager_;

        void SetUp() override
        {
            root_ = fs::canonical(temporaryRoot) /
                ("local_watch_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            fs::create_directories(root_ / "project" / "src");
            fs::create_directories(root_ / "downloads");
            std::ofstream{root_ / "project" / "src" / "main.cpp"} << "int main() {}";
            std::ofstream{root_ / "notes.txt"} << "notes";
            manager_ = std::make_unique<FileTracking::LocalWatchManager>(
                boost::asio::make_strand(ioc_.get_executor()), root_ / "downloads"
            );
        }

        void TearDown() override
        {
            manager_.reset();
            if (!root_.empty())
                fs::remove_all(root_);
        }

        /** @brief What the watches of recordingSink() reported, in order. */
        std::vector<FileTracking::ReportedChange> reported_{};

        auto watch(fs::path const& item)
        {
            return manager_->watch(
                Ids::generateWatchId(),
                item,
                std::string{Persistence::defaultTempFileSuffix},
                [](FileTracking::ReportedChange const&) {}
            );
        }

        FileTracking::ChangeSink recordingSink()
        {
            return [this](FileTracking::ReportedChange const& change)
            {
                reported_.push_back(change);
            };
        }

        auto watchRecording(fs::path const& item)
        {
            return manager_->watch(
                Ids::generateWatchId(), item, std::string{Persistence::defaultTempFileSuffix}, recordingSink()
            );
        }

        /**
         * @brief Lets the events from before the watch pass and forgets them, FSEvents delivers those too.
         */
        void settleEarlierEvents()
        {
            runFor(std::chrono::milliseconds{1000});
            reported_.clear();
        }

        /**
         * @brief Watches @p item without the manager, so the test can hand it changes, see LocalItemWatch::handleChange.
         */
        std::optional<FileTracking::LocalItemWatch> openRecording(fs::path const& item)
        {
            auto opened = FileTracking::LocalItemWatch::open(
                Ids::generateWatchId(),
                item,
                boost::asio::make_strand(ioc_.get_executor()),
                recordingSink(),
                std::string{Persistence::defaultTempFileSuffix},
                [](Ids::WatchId const&, fs::path const&)
                {
                    return true;
                }
            );
            if (!opened)
                return std::nullopt;
            return std::move(*opened);
        }

        std::size_t reportedCount(FileTracking::FileAction action, fs::path const& path) const
        {
            return static_cast<std::size_t>(std::ranges::count_if(
                reported_,
                [&](FileTracking::ReportedChange const& change)
                {
                    return change.action == action && change.path.lexically_normal() == path.lexically_normal();
                }
            ));
        }

        /**
         * @brief How many changes of the content of @p path were reported. FSEvents carries the creation of a file into
         *        its later events, efsw reports an edit as an addition and a modification then, they settle into the
         *        addition.
         */
        std::size_t reportedContentChanges(fs::path const& path) const
        {
            return reportedCount(FileTracking::FileAction::Added, path) +
                reportedCount(FileTracking::FileAction::Modified, path);
        }

        bool reportedRemoval() const
        {
            return std::ranges::any_of(
                reported_,
                [](FileTracking::ReportedChange const& change)
                {
                    return change.action == FileTracking::FileAction::Deleted;
                }
            );
        }

        FileTracking::DirectoryChange changeOf(
            FileTracking::FileAction action,
            std::string const& filename,
            std::string const& oldFilename = {}
        ) const
        {
            return FileTracking::DirectoryChange{
                .action = action,
                .directory = root_,
                .filename = filename,
                .oldFilename = oldFilename,
                .isDirectory = false,
            };
        }

        /**
         * @brief Runs the handlers until @p condition holds, at most for @p timeout.
         */
        bool runUntil(std::function<bool()> const& condition, std::chrono::milliseconds timeout = std::chrono::seconds{5})
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                ioc_.restart();
                ioc_.run_for(std::chrono::milliseconds{20});
                if (condition())
                    return true;
            }
            return false;
        }

        void runFor(std::chrono::milliseconds duration)
        {
            runUntil(
                []()
                {
                    return false;
                },
                duration
            );
        }
    };

    TEST_F(LocalWatchManagerTest, WatchingAFileReportsItAndItsDirectory)
    {
        const auto watched = watch(root_ / "project" / "src" / ".." / "src" / "main.cpp");

        ASSERT_TRUE(watched.has_value()) << watched.error();
        EXPECT_EQ((*watched)->item(), root_ / "project" / "src" / "main.cpp");
        EXPECT_EQ((*watched)->directory(), root_ / "project" / "src");
        EXPECT_FALSE((*watched)->isDirectory());
        EXPECT_TRUE((*watched)->watchId().isValid());
        EXPECT_EQ(manager_->find((*watched)->watchId()), *watched);
    }

    TEST_F(LocalWatchManagerTest, WatchingADirectoryWorks)
    {
        const auto watched = watch(root_ / "project");

        ASSERT_TRUE(watched.has_value()) << watched.error();
        EXPECT_TRUE((*watched)->isDirectory());
    }

    TEST_F(LocalWatchManagerTest, OverlappingWatchesAreRefused)
    {
        ASSERT_TRUE(watch(root_ / "project").has_value());

        EXPECT_FALSE(watch(root_ / "project").has_value());
        EXPECT_FALSE(watch(root_ / "project" / "src" / "main.cpp").has_value());
        EXPECT_FALSE(watch(root_).has_value());
        EXPECT_TRUE(watch(root_ / "notes.txt").has_value());
    }

    TEST_F(LocalWatchManagerTest, CheckingTellsWhatAWatchWouldRefuseWithoutWatching)
    {
        const auto checked = manager_->checkWatchable(root_ / "project" / "." / "src");
        ASSERT_TRUE(checked.has_value()) << checked.error();
        EXPECT_EQ(*checked, root_ / "project" / "src");
        ASSERT_TRUE(watch(root_ / "project").has_value());

        EXPECT_FALSE(manager_->checkWatchable(root_ / "project" / "src").has_value());
        EXPECT_FALSE(manager_->checkWatchable(root_ / "missing.txt").has_value());
        EXPECT_TRUE(manager_->checkWatchable(root_ / "notes.txt").has_value());
        EXPECT_TRUE(watch(root_ / "notes.txt").has_value());
    }

    TEST_F(LocalWatchManagerTest, TheTemporaryDownloadsDirectoryIsRefused)
    {
        std::ofstream{root_ / "downloads" / "copy.txt"} << "copy";

        EXPECT_FALSE(watch(root_ / "downloads" / "copy.txt").has_value());
        EXPECT_FALSE(watch(root_ / "downloads").has_value());
    }

    TEST_F(LocalWatchManagerTest, MissingItemsAndRootsAreRefused)
    {
        EXPECT_FALSE(watch(root_ / "missing.txt").has_value());
        EXPECT_FALSE(watch(root_.root_path()).has_value());
    }

    TEST_F(LocalWatchManagerTest, ARenamedItemIsFollowed)
    {
        const auto watched = watch(root_ / "notes.txt");
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();

        fs::rename(root_ / "notes.txt", root_ / "renamed.txt");

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == root_ / "renamed.txt";
            }
        ));
    }

    TEST_F(LocalWatchManagerTest, ARenamedDirectoryIsFollowed)
    {
        const auto watched = watch(root_ / "project");
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();

        fs::rename(root_ / "project", root_ / "renamed");

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == root_ / "renamed";
            }
        ));
        EXPECT_TRUE(manager_->find(watchId)->isDirectory());
    }

    TEST_F(LocalWatchManagerTest, AnItemRenamedTwiceIsFollowedToItsLastName)
    {
        const auto watched = watch(root_ / "notes.txt");
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();

        fs::rename(root_ / "notes.txt", root_ / "first.txt");
        fs::rename(root_ / "first.txt", root_ / "second.txt");

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == root_ / "second.txt";
            }
        ));
    }

    TEST_F(LocalWatchManagerTest, ARenameOntoAnotherWatchIsNotFollowed)
    {
        std::ofstream{root_ / "other.txt"} << "other";
        const auto watched = watch(root_ / "notes.txt");
        const auto other = watch(root_ / "other.txt");
        ASSERT_TRUE(watched.has_value()) << watched.error();
        ASSERT_TRUE(other.has_value()) << other.error();
        const auto watchId = (*watched)->watchId();
        const auto otherId = (*other)->watchId();

        fs::rename(root_ / "notes.txt", root_ / "other.txt");
        runUntil(
            []()
            {
                return false;
            },
            std::chrono::seconds{1}
        );

        EXPECT_EQ(manager_->find(watchId)->item(), root_ / "notes.txt");
        EXPECT_EQ(manager_->find(otherId)->item(), root_ / "other.txt");
    }

    TEST_F(LocalWatchManagerTest, ARenameOntoATemporaryNameIsNotFollowed)
    {
        const auto watched = watch(root_ / "project");
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();

        fs::rename(root_ / "project", root_ / "._project");

        EXPECT_FALSE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() != root_ / "project";
            },
            std::chrono::milliseconds{1000}
        ));
    }

    TEST_F(LocalWatchManagerTest, AnItemMovedAsideAndWrittenAnewStaysWatched)
    {
        const auto watched = watch(root_ / "notes.txt");
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();

        fs::rename(root_ / "notes.txt", root_ / "notes.txt~");
        std::ofstream{root_ / "notes.txt"} << "written anew";

        EXPECT_FALSE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() != root_ / "notes.txt";
            },
            std::chrono::milliseconds{1000}
        ));
    }

    TEST_F(LocalWatchManagerTest, AnItemMovedAsideAndWrittenAnewAfterTheSettleTimeTakesTheWatchBack)
    {
        const auto watched = manager_->watch(
            Ids::generateWatchId(), root_ / "notes.txt", std::string{Persistence::defaultTempFileSuffix}, recordingSink()
        );
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();

        fs::rename(root_ / "notes.txt", root_ / "notes.txt~");
        runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == root_ / "notes.txt~";
            }
        );
        runFor(std::chrono::milliseconds{300});
        std::ofstream{root_ / "notes.txt"} << "written anew";
        fs::remove(root_ / "notes.txt~");

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == root_ / "notes.txt" &&
                    reportedCount(FileTracking::FileAction::Added, root_ / "notes.txt") > 0u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(manager_->find(watchId)->item(), root_ / "notes.txt");
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Moved, root_ / "notes.txt~"), 1u);
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Moved, root_ / "notes.txt"), 1u);
        EXPECT_FALSE(reportedRemoval());
    }

    TEST_F(LocalWatchManagerTest, AFileAtTheOldNameLongAfterAFollowedRenameIsNotTheItem)
    {
        const auto watched = manager_->watch(
            Ids::generateWatchId(), root_ / "notes.txt", std::string{Persistence::defaultTempFileSuffix}, recordingSink()
        );
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();

        fs::rename(root_ / "notes.txt", root_ / "renamed.txt");
        ASSERT_TRUE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == root_ / "renamed.txt";
            }
        ));
        runFor(FileTracking::renameReturnTime);
        std::ofstream{root_ / "notes.txt"} << "another file";
        runFor(std::chrono::milliseconds{1000});

        EXPECT_EQ(manager_->find(watchId)->item(), root_ / "renamed.txt");
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Added, root_ / "notes.txt"), 0u);
    }

    TEST_F(LocalWatchManagerTest, AnItemRemovedAndWrittenAnewIsNotReportedRemoved)
    {
        const auto watched = manager_->watch(
            Ids::generateWatchId(), root_ / "notes.txt", std::string{Persistence::defaultTempFileSuffix}, recordingSink()
        );
        ASSERT_TRUE(watched.has_value()) << watched.error();

        fs::remove(root_ / "notes.txt");
        std::ofstream{root_ / "notes.txt"} << "written anew";

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedCount(FileTracking::FileAction::Added, root_ / "notes.txt") > 0u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_FALSE(reportedRemoval());
    }

    TEST_F(LocalWatchManagerTest, AnItemThatStaysAwayIsReportedRemoved)
    {
        const auto watched = manager_->watch(
            Ids::generateWatchId(), root_ / "notes.txt", std::string{Persistence::defaultTempFileSuffix}, recordingSink()
        );
        ASSERT_TRUE(watched.has_value()) << watched.error();

        fs::remove(root_ / "notes.txt");

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedCount(FileTracking::FileAction::Deleted, root_ / "notes.txt") == 1u;
            }
        ));
    }

    TEST_F(LocalWatchManagerTest, MissedChangesOfTheItemsDirectoryHaveTheItemLookedAtAnew)
    {
        auto watched = openRecording(root_ / "notes.txt");
        ASSERT_TRUE(watched.has_value());
        settleEarlierEvents();

        watched->handleChange(FileTracking::DirectoryChange{
            .action = FileTracking::FileAction::Missed,
            .directory = root_,
            .filename = {},
            .oldFilename = {},
            .isDirectory = true,
        });

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedCount(FileTracking::FileAction::Missed, root_ / "notes.txt") == 1u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_FALSE(reportedRemoval());
    }

    TEST_F(LocalWatchManagerTest, AnItemFoundGoneWhenItsOwnWatchMissedChangesIsReportedRemovedOnce)
    {
        const auto item = root_ / "project";
        auto watched = openRecording(item);
        ASSERT_TRUE(watched.has_value());
        settleEarlierEvents();

        fs::remove_all(item);
        watched->handleChange(FileTracking::DirectoryChange{
            .action = FileTracking::FileAction::Missed,
            .directory = item,
            .filename = {},
            .oldFilename = {},
            .isDirectory = true,
        });

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedCount(FileTracking::FileAction::Deleted, item) > 0u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        // Windows reports one of its own, the watched directory turned delete-pending.
        EXPECT_GE(reportedCount(FileTracking::FileAction::Missed, item), 1u);
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Deleted, item), 1u);
    }

    TEST_F(LocalWatchManagerTest, ARemovalReportedBeforeARenameOfTheItemIsNotReported)
    {
        auto watched = openRecording(root_ / "notes.txt");
        ASSERT_TRUE(watched.has_value());

        // Windows reports a case-only rename as the removal of the old name first.
        fs::rename(root_ / "notes.txt", root_ / "NOTES.txt");
        watched->handleChange(changeOf(FileTracking::FileAction::Deleted, "notes.txt"));
        watched->handleChange(changeOf(FileTracking::FileAction::Moved, "NOTES.txt", "notes.txt"));

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return watched->item() == root_ / "NOTES.txt";
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Moved, root_ / "NOTES.txt"), 1u);
        EXPECT_FALSE(reportedRemoval());
    }

    TEST_F(LocalWatchManagerTest, ARemovalReportedAfterARenameOfTheItemIsNotReported)
    {
        auto watched = openRecording(root_ / "notes.txt");
        ASSERT_TRUE(watched.has_value());

        fs::rename(root_ / "notes.txt", root_ / "NOTES.txt");
        watched->handleChange(changeOf(FileTracking::FileAction::Moved, "NOTES.txt", "notes.txt"));
        watched->handleChange(changeOf(FileTracking::FileAction::Deleted, "notes.txt"));

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return watched->item() == root_ / "NOTES.txt";
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Moved, root_ / "NOTES.txt"), 1u);
        EXPECT_FALSE(reportedRemoval());
    }

    TEST_F(LocalWatchManagerTest, UnwatchingFreesTheItem)
    {
        const auto watched = watch(root_ / "notes.txt");
        ASSERT_TRUE(watched.has_value());
        const auto watchId = (*watched)->watchId();

        manager_->unwatch(watchId);

        EXPECT_EQ(manager_->find(watchId), nullptr);
        EXPECT_TRUE(watch(root_ / "notes.txt").has_value());
        EXPECT_NO_THROW(manager_->unwatch(Ids::makeWatchId("unknown")));
    }

#ifdef __APPLE__
    TEST_F(LocalWatchManagerTest, ARemovedItemReportsNoChangeOfItsContentFirst)
    {
        const auto item = root_ / "notes.txt";
        const auto watched = watchRecording(item);
        ASSERT_TRUE(watched.has_value()) << watched.error();
        settleEarlierEvents();

        fs::remove(item);

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedCount(FileTracking::FileAction::Deleted, item) == 1u;
            }
        ));
        EXPECT_EQ(reportedContentChanges(item), 0u);
    }

    // Finder names in NFD and APFS keeps that spelling, efsw reports every name precomposed (NFC).
    TEST_F(LocalWatchManagerTest, AnItemInADecomposedDirectoryReportsItsEditsUnderItsSpelling)
    {
        const auto directory = root_ / Paths::nativeFromUtf8("Cafe\xCC\x81");
        const auto item = directory / "notes.txt";
        fs::create_directories(directory);
        std::ofstream{item} << "notes";
        const auto watched = watchRecording(item);
        ASSERT_TRUE(watched.has_value()) << watched.error();
        ASSERT_EQ((*watched)->item(), item);
        settleEarlierEvents();

        std::ofstream{item, std::ios::app} << " edited";

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedContentChanges(item) > 0u;
            }
        ));
    }

    TEST_F(LocalWatchManagerTest, AnItemInADecomposedDirectoryIsFollowedWhenRenamed)
    {
        const auto directory = root_ / Paths::nativeFromUtf8("Cafe\xCC\x81");
        fs::create_directories(directory);
        std::ofstream{directory / "notes.txt"} << "notes";
        const auto watched = watchRecording(directory / "notes.txt");
        ASSERT_TRUE(watched.has_value()) << watched.error();
        const auto watchId = (*watched)->watchId();
        settleEarlierEvents();

        fs::rename(directory / "notes.txt", directory / "renamed.txt");

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == directory / "renamed.txt";
            }
        ));
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Moved, directory / "renamed.txt"), 1u);
        EXPECT_FALSE(reportedRemoval());
    }

    TEST_F(LocalWatchManagerTest, AnItemInADecomposedDirectoryThatStaysAwayIsReportedRemoved)
    {
        const auto directory = root_ / Paths::nativeFromUtf8("Cafe\xCC\x81");
        const auto item = directory / "notes.txt";
        fs::create_directories(directory);
        std::ofstream{item} << "notes";
        const auto watched = watchRecording(item);
        ASSERT_TRUE(watched.has_value()) << watched.error();
        settleEarlierEvents();

        fs::remove(item);

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedCount(FileTracking::FileAction::Deleted, item) == 1u;
            }
        ));
    }

    TEST_F(LocalWatchManagerTest, AnItemNamedDecomposedReportsItsEditsAndIsFollowedWhenRenamed)
    {
        const auto item = root_ / Paths::nativeFromUtf8("cafe\xCC\x81.txt");
        std::ofstream{item} << "notes";
        const auto watched = watchRecording(item);
        ASSERT_TRUE(watched.has_value()) << watched.error();
        ASSERT_EQ((*watched)->item(), item);
        const auto watchId = (*watched)->watchId();
        settleEarlierEvents();

        std::ofstream{item, std::ios::app} << " edited";
        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedContentChanges(item) > 0u;
            }
        ));
        fs::rename(item, root_ / "renamed.txt");

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return manager_->find(watchId)->item() == root_ / "renamed.txt";
            }
        ));
        EXPECT_EQ(reportedCount(FileTracking::FileAction::Moved, root_ / "renamed.txt"), 1u);
        EXPECT_FALSE(reportedRemoval());
    }

    TEST_F(LocalWatchManagerTest, ADecomposedDirectoryReportsChangesBelowItUnderItsSpelling)
    {
        const auto item = root_ / Paths::nativeFromUtf8("Cafe\xCC\x81");
        fs::create_directories(item / "sub");
        std::ofstream{item / "sub" / "notes.txt"} << "notes";
        const auto watched = watchRecording(item);
        ASSERT_TRUE(watched.has_value()) << watched.error();
        ASSERT_TRUE((*watched)->isDirectory());
        settleEarlierEvents();

        std::ofstream{item / "sub" / "notes.txt", std::ios::app} << " edited";

        EXPECT_TRUE(runUntil(
            [&]()
            {
                return reportedContentChanges(item / "sub" / "notes.txt") > 0u;
            }
        ));
    }
#endif

    class DirectoryWatchServiceTest : public ::testing::Test
    {
      protected:
        fs::path root_;
        std::shared_ptr<FileTracking::DirectoryWatchService> service_ = FileTracking::DirectoryWatchService::shared();

        /**
         * @brief Collects the names a subscription reports, efsw calls it on its own thread.
         */
        struct Received
        {
            std::mutex mutex{};
            std::vector<std::string> filenames{};
            std::vector<FileTracking::DirectoryEvent> events{};

            FileTracking::DirectoryEventSink sink()
            {
                return [this](FileTracking::DirectoryEvent const& event)
                {
                    std::scoped_lock lock{mutex};
                    filenames.push_back(event.filename);
                    events.push_back(event);
                };
            }

            /**
             * @brief The move reported to the file @p filename, nothing when none was.
             */
            std::optional<FileTracking::DirectoryEvent> moveTo(std::string const& filename)
            {
                std::scoped_lock lock{mutex};
                const auto found = std::ranges::find_if(
                    events,
                    [&filename](FileTracking::DirectoryEvent const& event)
                    {
                        return event.action == FileTracking::FileAction::Moved &&
                            fs::path{event.filename}.filename() == filename;
                    }
                );
                if (found == events.end())
                    return std::nullopt;
                return *found;
            }

            bool has(std::string const& filename)
            {
                std::scoped_lock lock{mutex};
                return std::ranges::any_of(
                    filenames,
                    [&filename](std::string const& received)
                    {
                        return fs::path{received}.filename() == filename;
                    }
                );
            }

            /**
             * @brief The directories of the Missed events, in their order.
             */
            std::vector<fs::path> missed()
            {
                std::scoped_lock lock{mutex};
                std::vector<fs::path> directories;
                for (auto const& event : events)
                    if (event.action == FileTracking::FileAction::Missed)
                        directories.push_back(Paths::nativeFromUtf8(event.directory));
                return directories;
            }
        };

        void SetUp() override
        {
            root_ = fs::canonical(temporaryRoot) /
                ("watch_service_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            fs::create_directories(root_ / "sub");
        }

        void TearDown() override
        {
            fs::remove_all(root_);
        }

        static bool waitFor(std::function<bool()> const& condition, std::chrono::milliseconds timeout = std::chrono::seconds{5})
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (condition())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            return false;
        }
    };

    TEST_F(DirectoryWatchServiceTest, SubscribersOfOneDirectoryEachGetItsChanges)
    {
        Received first;
        Received second;
        const auto firstWatch = service_->subscribe(root_, false, first.sink());
        const auto secondWatch = service_->subscribe(root_, false, second.sink());
        ASSERT_TRUE(firstWatch.has_value());
        ASSERT_TRUE(secondWatch.has_value());

        std::ofstream{root_ / "shared.txt"} << "shared";

        EXPECT_TRUE(waitFor(
            [&]()
            {
                return first.has("shared.txt") && second.has("shared.txt");
            }
        ));
    }

    TEST_F(DirectoryWatchServiceTest, RecursiveSubscribersOfOneDirectoryEachGetItsChanges)
    {
        Received first;
        Received second;
        const auto firstWatch = service_->subscribe(root_, true, first.sink());
        const auto secondWatch = service_->subscribe(root_, true, second.sink());
        ASSERT_TRUE(firstWatch.has_value());
        ASSERT_TRUE(secondWatch.has_value());

        std::ofstream{root_ / "sub" / "deep.txt"} << "deep";

        EXPECT_TRUE(waitFor(
            [&]()
            {
                return first.has("deep.txt") && second.has("deep.txt");
            }
        ));
    }

    TEST_F(DirectoryWatchServiceTest, AMoveBetweenDirectoriesOfARecursiveWatchIsOneMove)
    {
        fs::create_directories(root_ / "other");
        std::ofstream{root_ / "sub" / "moved.txt"} << "moved";
        Received received;
        const auto watch = service_->subscribe(root_, true, received.sink());
        ASSERT_TRUE(watch.has_value());

        fs::rename(root_ / "sub" / "moved.txt", root_ / "other" / "moved.txt");

#ifdef __APPLE__
        // FSEvents pairs the two halves only when they arrive together.
        EXPECT_TRUE(waitFor(
            [&]()
            {
                return received.has("moved.txt");
            }
        ));
#else
        ASSERT_TRUE(waitFor(
            [&]()
            {
                return received.moveTo("moved.txt").has_value();
            }
        ));
        const auto move = *received.moveTo("moved.txt");
        EXPECT_EQ(
            fs::path{Paths::nativeFromUtf8(move.oldFilename)}.lexically_normal(),
            (root_ / "sub" / "moved.txt").lexically_normal()
        );
#endif
    }

    TEST_F(DirectoryWatchServiceTest, RecursiveAndPlainWatchesOfOtherDirectoriesBothDeliver)
    {
        fs::create_directories(root_ / "plain");
        Received recursive;
        Received plain;
        const auto recursiveWatch = service_->subscribe(root_ / "sub", true, recursive.sink());
        const auto plainWatch = service_->subscribe(root_ / "plain", false, plain.sink());
        ASSERT_TRUE(recursiveWatch.has_value());
        ASSERT_TRUE(plainWatch.has_value());

        fs::create_directories(root_ / "sub" / "nested");
        std::ofstream{root_ / "sub" / "recursive.txt"} << "recursive";
        std::ofstream{root_ / "plain" / "plain.txt"} << "plain";

        EXPECT_TRUE(waitFor(
            [&]()
            {
                return recursive.has("recursive.txt") && plain.has("plain.txt");
            }
        ));
        EXPECT_FALSE(recursive.has("plain.txt"));
        EXPECT_FALSE(plain.has("recursive.txt"));
    }

    TEST_F(DirectoryWatchServiceTest, ADirectoryIsWatchedOneWayOnly)
    {
        Received plain;
        Received recursive;
        const auto plainWatch = service_->subscribe(root_, false, plain.sink());
        ASSERT_TRUE(plainWatch.has_value());
        EXPECT_FALSE(service_->subscribe(root_, true, recursive.sink()).has_value());

        std::ofstream{root_ / "direct.txt"} << "direct";

        EXPECT_TRUE(waitFor(
            [&]()
            {
                return plain.has("direct.txt");
            }
        ));
        EXPECT_FALSE(recursive.has("direct.txt"));
    }

    TEST_F(DirectoryWatchServiceTest, AnEndedSubscriptionGetsNothingAndTheDirectoryCanBeWatchedAgain)
    {
        Received ended;
        auto endedWatch = service_->subscribe(root_, true, ended.sink());
        ASSERT_TRUE(endedWatch.has_value());
        endedWatch->release();

        Received again;
        const auto againWatch = service_->subscribe(root_, true, again.sink());
        ASSERT_TRUE(againWatch.has_value());
        std::ofstream{root_ / "sub" / "again.txt"} << "again";

        EXPECT_TRUE(waitFor(
            [&]()
            {
                return again.has("again.txt");
            }
        ));
        EXPECT_FALSE(ended.has("again.txt"));
    }

    TEST_F(DirectoryWatchServiceTest, MissedChangesReachTheSubscriptionsOfTheWatchCoveringTheirDirectory)
    {
        fs::create_directories(root_ / "sub" / "deep");
        fs::create_directories(root_ / "plain");
        Received recursive;
        Received plain;
        const auto recursiveWatch = service_->subscribe(root_ / "sub", true, recursive.sink());
        const auto plainWatch = service_->subscribe(root_ / "plain", false, plain.sink());
        ASSERT_TRUE(recursiveWatch.has_value());
        ASSERT_TRUE(plainWatch.has_value());

        service_->reportMissed(root_ / "sub" / "deep");

        EXPECT_EQ(recursive.missed(), std::vector<fs::path>{root_ / "sub" / "deep"});
        EXPECT_TRUE(plain.missed().empty());
    }

    TEST_F(DirectoryWatchServiceTest, TheWatchesOfDeletedDirectoriesEnd)
    {
        fs::create_directories(root_ / "plain" / "inner");
        fs::create_directories(root_ / "recursive" / "inner");
        auto service = std::make_shared<FileTracking::DirectoryWatchService>();
        auto plain = std::make_shared<Received>();
        auto recursive = std::make_shared<Received>();
        auto plainWatch = service->subscribe(root_ / "plain", false, plain->sink());
        auto recursiveWatch = service->subscribe(root_ / "recursive", true, recursive->sink());
        ASSERT_TRUE(plainWatch.has_value());
        ASSERT_TRUE(recursiveWatch.has_value());

        // FSEvents reports nothing below a watched directory removed with its contents, so the inner ones go first.
        fs::remove(root_ / "plain" / "inner");
        fs::remove(root_ / "recursive" / "inner");
        EXPECT_TRUE(waitFor(
            [&]()
            {
                return plain->has("inner") && recursive->has("inner");
            }
        ));
        fs::remove(root_ / "plain");
        fs::remove(root_ / "recursive");
        // Gives the watchers time to find their directories gone.
        std::this_thread::sleep_for(std::chrono::milliseconds{500});

        // efsw 1.7.2 waits forever on Windows for a read that a deleted directory never completes.
        auto ended = std::make_shared<std::promise<void>>();
        auto endedFuture = ended->get_future();
        std::thread{
            [ended,
                service = std::move(service),
                plain,
                recursive,
                plainWatch = std::move(plainWatch),
                recursiveWatch = std::move(recursiveWatch)]() mutable
            {
                recursiveWatch.reset();
                plainWatch.reset();
                service.reset();
                ended->set_value();
            }
        }.detach();
        EXPECT_EQ(endedFuture.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    }

    TEST_F(DirectoryWatchServiceTest, WatchesComingAndGoingCostOthersNoChanges)
    {
        constexpr int otherCount = 20;
        constexpr int fileCount = 100;
        for (int index = 0; index < otherCount; ++index)
            fs::create_directories(root_ / ("other" + std::to_string(index)));
        fs::create_directories(root_ / "watched");
        Received received;
        const auto watched = service_->subscribe(root_ / "watched", false, received.sink());
        ASSERT_TRUE(watched.has_value());

        std::atomic<bool> writing{true};
        std::thread comingAndGoing{[&]()
                                   {
                                       while (writing)
                                       {
                                           std::vector<std::optional<FileTracking::InstanceWatch>> others;
                                           for (int index = 0; index < otherCount; ++index)
                                               others.push_back(service_->subscribe(
                                                   root_ / ("other" + std::to_string(index)),
                                                   false,
                                                   [](FileTracking::DirectoryEvent const&) {}
                                               ));
                                           others.clear();
                                           std::this_thread::sleep_for(std::chrono::milliseconds{10});
                                       }
                                   }};
        for (int index = 0; index < fileCount; ++index)
        {
            std::ofstream{root_ / "watched" / ("file" + std::to_string(index) + ".txt")} << index;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        writing = false;
        comingAndGoing.join();

        const auto missing = [&]()
        {
            std::string names;
            for (int index = 0; index < fileCount; ++index)
                if (const auto name = "file" + std::to_string(index) + ".txt"; !received.has(name))
                    names += name + " ";
            return names;
        };
        EXPECT_TRUE(waitFor(
            [&]()
            {
                return missing().empty();
            }
        )) << "missing: " << missing();
    }

    class DirectoryWatcherTest : public ::testing::Test
    {
      protected:
        boost::asio::io_context ioc_;
        fs::path root_;
        std::vector<FileTracking::DirectoryChange> changes_{};
        std::unique_ptr<FileTracking::DirectoryWatcher> watcher_;

        void SetUp() override
        {
            root_ = fs::canonical(temporaryRoot) /
                ("directory_watcher_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            fs::create_directories(root_);
            watcher_ = std::make_unique<FileTracking::DirectoryWatcher>(
                boost::asio::make_strand(ioc_.get_executor()),
                [this](FileTracking::DirectoryChange const& change)
                {
                    changes_.push_back(change);
                }
            );
        }

        void TearDown() override
        {
            watcher_.reset();
            if (!root_.empty())
                fs::remove_all(root_);
        }

        /**
         * @brief Runs the handlers until @p condition holds, at most for @p timeout.
         */
        bool runUntil(std::function<bool()> const& condition, std::chrono::milliseconds timeout = std::chrono::seconds{5})
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                ioc_.restart();
                ioc_.run_for(std::chrono::milliseconds{20});
                if (condition())
                    return true;
            }
            return false;
        }

        /**
         * @brief Whether a change of @p filename came that the trackers act on, which a directory reported as
         *        modified is not (see filterFileChange). NTFS writes a directory's times into its parent's entry when
         *        a handle to it closes, so a watch of the parent reports it whenever something opens it, efsw too.
         */
        bool reported(std::string const& filename) const
        {
            return std::ranges::any_of(
                changes_,
                [&filename](FileTracking::DirectoryChange const& change)
                {
                    return change.filename == filename &&
                        !(change.action == FileTracking::FileAction::Modified && change.isDirectory);
                }
            );
        }
    };

    TEST_F(DirectoryWatcherTest, ChangesMadeBeforeTheWatchBeganAreNotReported)
    {
        fs::create_directories(root_ / "fresh" / "inner");
        std::ofstream{root_ / "fresh" / "inner" / "before.txt"} << "before";
        const auto plain = watcher_->add(root_, /*recursive=*/false);
        const auto recursive = watcher_->add(root_ / "fresh", /*recursive=*/true);
        ASSERT_TRUE(plain.has_value());
        ASSERT_TRUE(recursive.has_value());

        std::ofstream{root_ / "after.txt"} << "after";
        std::ofstream{root_ / "fresh" / "inner" / "later.txt"} << "later";

        EXPECT_TRUE(runUntil(
            [this]()
            {
                return reported("after.txt") && reported("later.txt");
            }
        ));
        runUntil(
            []()
            {
                return false;
            },
            std::chrono::milliseconds{500}
        );
        EXPECT_FALSE(reported("fresh"));
        EXPECT_FALSE(reported("before.txt"));
    }

    TEST_F(DirectoryWatcherTest, MissedChangesPassTheFilterAfterTheChangesBeforeThem)
    {
        const auto watch = watcher_->add(
            root_,
            /*recursive=*/true,
            [](FileTracking::DirectoryEvent const& event)
            {
                return event.filename == "before.txt";
            }
        );
        ASSERT_TRUE(watch.has_value());
        // Subscribed after the watcher, efsw's thread hands an event to it once the watcher has it.
        const auto service = FileTracking::DirectoryWatchService::shared();
        std::atomic<bool> handedOn{false};
        const auto observer = service->subscribe(
            root_,
            /*recursive=*/true,
            [&handedOn](FileTracking::DirectoryEvent const& event)
            {
                if (event.filename == "before.txt")
                    handedOn = true;
            }
        );
        ASSERT_TRUE(observer.has_value());

        std::ofstream{root_ / "before.txt"} << "before";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!handedOn && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        ASSERT_TRUE(handedOn);
        service->reportMissed(root_);

        const auto isMissed = [](FileTracking::DirectoryChange const& change)
        {
            return change.action == FileTracking::FileAction::Missed;
        };
        ASSERT_TRUE(runUntil(
            [&]()
            {
                return std::ranges::any_of(changes_, isMissed);
            }
        ));
        const auto missed = std::ranges::find_if(changes_, isMissed);
        EXPECT_EQ(missed->path(), root_);
        const auto before = std::ranges::find_if(
            changes_,
            [](FileTracking::DirectoryChange const& change)
            {
                return change.filename == "before.txt";
            }
        );
        EXPECT_LT(before, missed);
    }

    TEST_F(DirectoryWatcherTest, AnEntryWrittenBeforeAndRenamedAfterTheWatchBeganIsReported)
    {
        std::ofstream{root_ / "old.txt"} << "old";
        fs::create_directories(root_ / "folder");
        const auto watch = watcher_->add(root_, /*recursive=*/false);
        ASSERT_TRUE(watch.has_value());

        fs::rename(root_ / "old.txt", root_ / "new.txt");
        fs::rename(root_ / "folder", root_ / "renamed");

        EXPECT_TRUE(runUntil(
            [this]()
            {
                return reported("new.txt") && reported("renamed");
            }
        ));
    }
}
