#pragma once

#include <backend/sftp/local_delete_operation.hpp>
#include <paths/native.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#    include <unistd.h>
#endif

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class LocalDeleteOperationTests : public ::testing::Test
    {
      protected:
        struct Progress
        {
            std::string item;
            std::uint64_t current;
            std::uint64_t total;
        };

        static Paths::LocalPath local(std::filesystem::path const& path)
        {
            return Paths::LocalPath::make(path).value();
        }

        LocalDeleteOperation::LocalDeleteOperationOptions options(std::vector<Paths::LocalPath> targets, bool recursive)
        {
            return {
                .itemsRemovedProgress =
                    [this](std::string const& item, std::uint64_t current, std::uint64_t total)
                {
                    progress_.push_back({item, current, total});
                },
                .scanProgress =
                    [this](std::uint64_t, std::uint64_t entries)
                {
                    scannedEntries_ = entries;
                },
                .targets = std::move(targets),
                .recursive = recursive,
            };
        }

        /**
         * @brief Works @p operation until it ends, at most @p maximumSteps times.
         */
        static std::expected<Operation::WorkStatus, Operation::Error>
        workToEnd(LocalDeleteOperation& operation, int maximumSteps = 1000)
        {
            for (int step = 0; step < maximumSteps; ++step)
            {
                auto result = operation.work();
                if (!result || *result == Operation::WorkStatus::Complete)
                    return result;
            }
            return Operation::WorkStatus::MoreWork;
        }

        void makeFiles(std::filesystem::path const& directory, int count)
        {
            std::filesystem::create_directories(directory);
            for (int index = 0; index < count; ++index)
                std::ofstream{directory / ("file" + std::to_string(index) + ".txt")} << index;
        }

        static bool makeDirectoryLink(std::filesystem::path const& link, std::filesystem::path const& target)
        {
            std::error_code error;
            std::filesystem::create_directory_symlink(target, link, error);
            return !error;
        }

        Utility::TemporaryDirectory root_{temporaryRoot, true};
        std::vector<Progress> progress_{};
        std::uint64_t scannedEntries_{0};
    };

    TEST_F(LocalDeleteOperationTests, CommonParentIsTheDeepestSharedDirectory)
    {
        const auto path = [](std::string const& text)
        {
            return Paths::LocalPath::fromUtf8(text, Paths::LocalPlatform::Posix).value();
        };
        const std::vector<Paths::LocalPath> siblings{path("/a/b/c"), path("/a/b/d/e")};
        EXPECT_EQ(LocalDeleteOperation::commonParentOf(siblings), path("/a/b"));
        const std::vector<Paths::LocalPath> single{path("/a/b")};
        EXPECT_EQ(LocalDeleteOperation::commonParentOf(single), path("/a"));
        const std::vector<Paths::LocalPath> nested{path("/a/b/c"), path("/a/b")};
        EXPECT_EQ(LocalDeleteOperation::commonParentOf(nested), path("/a"));
        const std::vector<Paths::LocalPath> apart{path("/a/x"), path("/b/y")};
        EXPECT_EQ(LocalDeleteOperation::commonParentOf(apart), path("/"));
        const std::vector<Paths::LocalPath> root{path("/")};
        EXPECT_EQ(LocalDeleteOperation::commonParentOf(root), path("/"));
        EXPECT_FALSE(LocalDeleteOperation::commonParentOf({}));
    }

    TEST_F(LocalDeleteOperationTests, ALinkTargetIsRemovedAsTheLinkOnly)
    {
        const auto target = root_.path() / "target";
        makeFiles(target, 2);
        ASSERT_TRUE(makeDirectoryLink(root_.path() / "link", target));
        std::filesystem::create_symlink(target / "file0.txt", root_.path() / "fileLink");

        for (bool const recursive : {false, true})
        {
            if (recursive)
            {
                ASSERT_TRUE(makeDirectoryLink(root_.path() / "link", target));
                std::filesystem::create_symlink(target / "file0.txt", root_.path() / "fileLink");
            }
            LocalDeleteOperation operation{
                options({local(root_.path() / "link"), local(root_.path() / "fileLink")}, recursive)
            };
            const auto result = workToEnd(operation);
            ASSERT_TRUE(result.has_value()) << result.error().toString();
            EXPECT_EQ(*result, Operation::WorkStatus::Complete);
            EXPECT_TRUE(operation.getFailed().empty());
            EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(root_.path() / "link")));
            EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(root_.path() / "fileLink")));
            EXPECT_TRUE(std::filesystem::exists(target / "file0.txt")) << "recursive: " << recursive;
            EXPECT_TRUE(std::filesystem::exists(target / "file1.txt")) << "recursive: " << recursive;
        }
    }

    TEST_F(LocalDeleteOperationTests, AnItemAlreadyGoneCountsAsDeleted)
    {
        std::ofstream{root_.path() / "present.txt"} << "present";
        LocalDeleteOperation operation{
            options({local(root_.path() / "gone.txt"), local(root_.path() / "present.txt")}, false)
        };

        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_EQ(operation.state(), SharedData::OperationState::Completed);
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "present.txt"));
        ASSERT_FALSE(progress_.empty());
        EXPECT_EQ(progress_.back().current, 2u);
        EXPECT_EQ(progress_.back().total, 2u);
    }

    TEST_F(LocalDeleteOperationTests, EveryTargetFailingFailsTheOperation)
    {
        makeFiles(root_.path() / "first", 1);
        makeFiles(root_.path() / "second", 1);
        LocalDeleteOperation operation{options({local(root_.path() / "first"), local(root_.path() / "second")}, false)};

        const auto result = workToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::DeleteFailed);
        EXPECT_EQ(operation.state(), SharedData::OperationState::Failed);
        ASSERT_EQ(operation.getFailed().size(), 2u);
        for (auto const& [path, error] : operation.getFailed())
            EXPECT_EQ(error.extraInfo.value_or("").find(path), std::string::npos) << "the path is named once";
        EXPECT_TRUE(std::filesystem::exists(root_.path() / "first" / "file0.txt"));
    }

    TEST_F(LocalDeleteOperationTests, CancelStopsBetweenEntries)
    {
        makeFiles(root_.path() / "many", 20);
        auto settings = options({local(root_.path() / "many")}, true);
        settings.stepBudget = std::chrono::milliseconds{0};
        LocalDeleteOperation operation{std::move(settings)};

        // One entry per step: the count walks into the directory and over the first file first.
        for (int step = 0; step < 25; ++step)
            ASSERT_EQ(operation.work(), Operation::WorkStatus::MoreWork);
        ASSERT_TRUE(operation.cancel(true).has_value());
        const auto afterCancel = operation.work();
        ASSERT_FALSE(afterCancel.has_value());
        EXPECT_EQ(afterCancel.error().type, Operation::ErrorType::CannotWorkCanceledOperation);

        std::size_t left = 0;
        for ([[maybe_unused]] auto const& entry : std::filesystem::directory_iterator{root_.path() / "many"})
            ++left;
        EXPECT_GT(left, 0u);
        EXPECT_LT(left, 20u) << "some files went before the cancel";
    }

    TEST_F(LocalDeleteOperationTests, AnUnworkedDeleteHoldsAndThenContinues)
    {
        makeFiles(root_.path() / "paused" / "inner", 10);
        makeFiles(root_.path() / "paused", 10);
        auto settings = options({local(root_.path() / "paused")}, true);
        settings.stepBudget = std::chrono::milliseconds{0};
        settings.progressInterval = std::chrono::hours{1};
        LocalDeleteOperation operation{std::move(settings)};

        for (int step = 0; step < 30; ++step)
            ASSERT_EQ(operation.work(), Operation::WorkStatus::MoreWork);
        EXPECT_EQ(scannedEntries_, 22u) << "two directories and twenty files";
        EXPECT_TRUE(std::filesystem::exists(root_.path() / "paused")) << "a paused queue does not work it further";

        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "paused"));
        EXPECT_LE(progress_.size(), 2u) << "progress is throttled, plus the final update";
        ASSERT_FALSE(progress_.empty());
        EXPECT_EQ(progress_.back().current, 22u);
        EXPECT_EQ(progress_.back().total, 22u);
    }

    TEST_F(LocalDeleteOperationTests, RecursiveDeleteRemovesNestedLinksWithoutFollowing)
    {
        const auto outside = root_.path() / "outside";
        makeFiles(outside, 3);
        const auto tree = root_.path() / "tree";
        makeFiles(tree / "a" / "b", 2);
        ASSERT_TRUE(makeDirectoryLink(tree / "a" / "b" / "out", outside));
        ASSERT_TRUE(makeDirectoryLink(tree / "top", outside));
        std::filesystem::create_symlink(outside / "file0.txt", tree / "a" / "fileLink");

        LocalDeleteOperation operation{options({local(tree)}, true)};
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_FALSE(std::filesystem::exists(tree));
        for (int index = 0; index < 3; ++index)
            EXPECT_TRUE(std::filesystem::exists(outside / ("file" + std::to_string(index) + ".txt")));
        EXPECT_EQ(scannedEntries_, 8u) << "tree, a, b, two files and three links";
        ASSERT_FALSE(progress_.empty());
        EXPECT_EQ(progress_.back().current, 8u);
    }

#ifndef _WIN32
    TEST_F(LocalDeleteOperationTests, AnUndeletableItemIsListedByItsOwnPath)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root removes entries of read-only directories";

        const auto tree = root_.path() / "stuck";
        makeFiles(tree / "locked", 1);
        makeFiles(tree, 3);
        std::filesystem::permissions(
            tree / "locked", std::filesystem::perms::owner_write, std::filesystem::perm_options::remove
        );

        LocalDeleteOperation operation{options({local(tree)}, true)};
        const auto result = workToEnd(operation);
        std::filesystem::permissions(
            tree / "locked", std::filesystem::perms::owner_write, std::filesystem::perm_options::add
        );

        ASSERT_TRUE(result.has_value()) << "a partial success completes";
        ASSERT_EQ(operation.getFailed().size(), 1u);
        EXPECT_EQ(operation.getFailed().front().first, Paths::nativeToUtf8(tree / "locked" / "file0.txt"));
        EXPECT_FALSE(std::filesystem::exists(tree / "file0.txt"));
        EXPECT_TRUE(std::filesystem::exists(tree / "locked" / "file0.txt"));
    }
#endif
}
