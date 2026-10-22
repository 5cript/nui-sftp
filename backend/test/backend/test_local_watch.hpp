#pragma once

#include <backend/file_tracking/directory_watch_service.hpp>
#include <backend/file_tracking/directory_watcher.hpp>
#include <backend/file_tracking/file_states.hpp>

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

            FileTracking::DirectoryEventSink sink()
            {
                return [this](FileTracking::DirectoryEvent const& event)
                {
                    std::scoped_lock lock{mutex};
                    filenames.push_back(event.filename);
                };
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
