#pragma once

#include <backend/sftp/local_copy_operation.hpp>
#include <paths/native.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class LocalCopyOperationTests : public ::testing::Test
    {
      protected:
        static Paths::LocalPath local(std::filesystem::path const& path)
        {
            return Paths::LocalPath::make(path).value();
        }

        static SharedData::LocalCopyEntry
        entry(std::filesystem::path const& source, std::filesystem::path const& destination, bool mayOverwrite = false)
        {
            return {.source = local(source), .destination = local(destination), .mayOverwrite = mayOverwrite};
        }

        LocalCopyOperation::LocalCopyOperationOptions
        options(std::vector<SharedData::LocalCopyEntry> entries, bool move)
        {
            return {
                .progress =
                    [this](LocalCopyOperation::Progress const& progress)
                {
                    progress_.push_back(progress);
                },
                .scanProgress =
                    [this](std::uint64_t bytes, std::uint64_t entries)
                {
                    scannedBytes_ = bytes;
                    scannedEntries_ = entries;
                },
                .entries = std::move(entries),
                .move = move,
            };
        }

        /**
         * @brief Works @p operation until it ends, at most @p maximumSteps times.
         */
        static std::expected<Operation::WorkStatus, Operation::Error>
        workToEnd(LocalCopyOperation& operation, int maximumSteps = 10000)
        {
            for (int step = 0; step < maximumSteps; ++step)
            {
                auto result = operation.work();
                if (!result || *result == Operation::WorkStatus::Complete)
                    return result;
            }
            return Operation::WorkStatus::MoreWork;
        }

        static void write(std::filesystem::path const& path, std::string const& content)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream{path, std::ios::binary} << content;
        }

        static std::string read(std::filesystem::path const& path)
        {
            std::ifstream stream{path, std::ios::binary};
            return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }

        static bool makeDirectoryLink(std::filesystem::path const& link, std::filesystem::path const& target)
        {
            std::error_code error;
            std::filesystem::create_directory_symlink(target, link, error);
            return !error;
        }

        static std::size_t countEntries(std::filesystem::path const& directory)
        {
            std::size_t count = 0;
            for ([[maybe_unused]] auto const& item : std::filesystem::directory_iterator{directory})
                ++count;
            return count;
        }

        Utility::TemporaryDirectory root_{temporaryRoot, true};
        std::vector<LocalCopyOperation::Progress> progress_{};
        std::uint64_t scannedBytes_{0};
        std::uint64_t scannedEntries_{0};
    };

    TEST_F(LocalCopyOperationTests, SameOrBelowComparesWholeComponents)
    {
        EXPECT_TRUE(LocalCopyOperation::isSameOrBelow("/a/b", "/a/b"));
        EXPECT_TRUE(LocalCopyOperation::isSameOrBelow("/a/b/c", "/a/b/"));
        EXPECT_TRUE(LocalCopyOperation::isSameOrBelow("/a/b/./c", "/a/b"));
        EXPECT_FALSE(LocalCopyOperation::isSameOrBelow("/a/bc", "/a/b"));
        EXPECT_FALSE(LocalCopyOperation::isSameOrBelow("/a", "/a/b"));
        EXPECT_FALSE(LocalCopyOperation::isSameOrBelow("/a/b/../c", "/a/b"));
    }

    TEST_F(LocalCopyOperationTests, CopiesATreeAndKeepsTheSource)
    {
        const auto source = root_.path() / "source";
        write(source / "top.txt", "top");
        write(source / "inner" / "deep" / "deep.txt", std::string(3000, 'x'));
        std::filesystem::create_directories(source / "empty");
        const auto modified = std::filesystem::last_write_time(source / "top.txt") - std::chrono::hours{24};
        std::filesystem::last_write_time(source / "top.txt", modified);
        std::filesystem::create_directories(root_.path() / "target");

        LocalCopyOperation operation{options({entry(source, root_.path() / "target" / "source")}, false)};
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(*result, Operation::WorkStatus::Complete);
        EXPECT_TRUE(operation.getFailed().empty());

        const auto copy = root_.path() / "target" / "source";
        EXPECT_EQ(read(copy / "top.txt"), "top");
        EXPECT_EQ(read(copy / "inner" / "deep" / "deep.txt"), std::string(3000, 'x'));
        EXPECT_TRUE(std::filesystem::is_directory(copy / "empty"));
        EXPECT_EQ(std::filesystem::last_write_time(copy / "top.txt"), modified);
        EXPECT_EQ(countEntries(copy), 3u) << "no partial file is left behind";
        EXPECT_EQ(read(source / "top.txt"), "top");

        EXPECT_EQ(scannedEntries_, 6u) << "source, top.txt, inner, deep, deep.txt and empty";
        EXPECT_EQ(scannedBytes_, 3003u);
        ASSERT_FALSE(progress_.empty());
        EXPECT_EQ(progress_.back().entriesDone, 6u);
        EXPECT_EQ(progress_.back().entriesTotal, 6u);
        EXPECT_EQ(progress_.back().bytesDone, 3003u);
        EXPECT_EQ(progress_.back().bytesTotal, 3003u);
    }

    TEST_F(LocalCopyOperationTests, LinksAreCopiedAsLinks)
    {
        const auto outside = root_.path() / "outside";
        write(outside / "file.txt", "outside");
        const auto source = root_.path() / "source";
        std::filesystem::create_directories(source);
        if (!makeDirectoryLink(source / "directoryLink", outside))
            GTEST_SKIP() << "links cannot be created here";
        std::filesystem::create_symlink(outside / "file.txt", source / "fileLink");

        LocalCopyOperation operation{options({entry(source, root_.path() / "copy")}, false)};
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());

        const auto copy = root_.path() / "copy";
        EXPECT_TRUE(std::filesystem::is_symlink(copy / "directoryLink"));
        EXPECT_TRUE(std::filesystem::is_symlink(copy / "fileLink"));
        EXPECT_EQ(std::filesystem::read_symlink(copy / "fileLink"), outside / "file.txt");
        EXPECT_EQ(countEntries(outside), 1u) << "nothing is written through a link";
    }

    TEST_F(LocalCopyOperationTests, ACopyOntoItselfTakesAFreeName)
    {
        write(root_.path() / "report.txt", "report");
        write(root_.path() / "folder" / "inside.txt", "inside");

        LocalCopyOperation first{options(
            {entry(root_.path() / "report.txt", root_.path() / "report.txt"),
             entry(root_.path() / "folder", root_.path() / "folder")},
            false
        )};
        ASSERT_TRUE(workToEnd(first).has_value());
        LocalCopyOperation second{options({entry(root_.path() / "report.txt", root_.path() / "report.txt")}, false)};
        ASSERT_TRUE(workToEnd(second).has_value());

        EXPECT_EQ(read(root_.path() / "report (copy).txt"), "report");
        EXPECT_EQ(read(root_.path() / "report (copy 2).txt"), "report");
        EXPECT_EQ(read(root_.path() / "folder (copy)" / "inside.txt"), "inside");
        EXPECT_EQ(read(root_.path() / "report.txt"), "report");
    }

    TEST_F(LocalCopyOperationTests, ADirectoryIsNotCopiedIntoItself)
    {
        write(root_.path() / "folder" / "inside.txt", "inside");

        for (bool const move : {false, true})
        {
            LocalCopyOperation operation{
                options({entry(root_.path() / "folder", root_.path() / "folder" / "folder")}, move)
            };
            const auto result = workToEnd(operation);
            ASSERT_FALSE(result.has_value()) << "move: " << move;
            EXPECT_EQ(result.error().type, Operation::ErrorType::CopyFailed);
            ASSERT_EQ(operation.getFailed().size(), 1u);
            EXPECT_EQ(operation.getFailed().front().second.type, Operation::ErrorType::CopyIntoItself);
            EXPECT_EQ(countEntries(root_.path() / "folder"), 1u);
        }
    }

    TEST_F(LocalCopyOperationTests, AnExistingDestinationIsKeptWithoutConsent)
    {
        write(root_.path() / "source.txt", "new");
        write(root_.path() / "target" / "source.txt", "old");
        write(root_.path() / "other.txt", "other");

        LocalCopyOperation operation{options(
            {entry(root_.path() / "source.txt", root_.path() / "target" / "source.txt"),
             entry(root_.path() / "other.txt", root_.path() / "target" / "other.txt")},
            true
        )};
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << "the other entry arrived";
        ASSERT_EQ(operation.getFailed().size(), 1u);
        EXPECT_EQ(operation.getFailed().front().second.type, Operation::ErrorType::FileExists);
        EXPECT_EQ(read(root_.path() / "target" / "source.txt"), "old");
        EXPECT_EQ(read(root_.path() / "source.txt"), "new");
        EXPECT_EQ(read(root_.path() / "target" / "other.txt"), "other");
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "other.txt"));
    }

    TEST_F(LocalCopyOperationTests, ConsentReplacesAFileAndMergesADirectory)
    {
        write(root_.path() / "source.txt", "new");
        write(root_.path() / "target" / "source.txt", "old");
        write(root_.path() / "folder" / "a.txt", "new a");
        write(root_.path() / "target" / "folder" / "a.txt", "old a");
        write(root_.path() / "target" / "folder" / "b.txt", "old b");

        LocalCopyOperation operation{options(
            {entry(root_.path() / "source.txt", root_.path() / "target" / "source.txt", true),
             entry(root_.path() / "folder", root_.path() / "target" / "folder", true)},
            false
        )};
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_EQ(read(root_.path() / "target" / "source.txt"), "new");
        EXPECT_EQ(read(root_.path() / "target" / "folder" / "a.txt"), "new a");
        EXPECT_EQ(read(root_.path() / "target" / "folder" / "b.txt"), "old b");
    }

    TEST_F(LocalCopyOperationTests, AFileAndADirectoryNeverReplaceEachOther)
    {
        write(root_.path() / "name", "file");
        write(root_.path() / "target" / "name" / "inside.txt", "inside");

        LocalCopyOperation operation{options({entry(root_.path() / "name", root_.path() / "target" / "name", true)}, true)};
        ASSERT_FALSE(workToEnd(operation).has_value());
        ASSERT_EQ(operation.getFailed().size(), 1u);
        EXPECT_EQ(operation.getFailed().front().second.type, Operation::ErrorType::OperationNotPossibleOnFileType);
        EXPECT_EQ(read(root_.path() / "name"), "file");
        EXPECT_EQ(read(root_.path() / "target" / "name" / "inside.txt"), "inside");
    }

    TEST_F(LocalCopyOperationTests, AMoveRenamesWithoutScanning)
    {
        write(root_.path() / "file.txt", "file");
        write(root_.path() / "folder" / "inner" / "inside.txt", "inside");
        std::filesystem::create_directories(root_.path() / "target");

        LocalCopyOperation operation{options(
            {entry(root_.path() / "file.txt", root_.path() / "target" / "file.txt"),
             entry(root_.path() / "folder", root_.path() / "target" / "folder")},
            true
        )};
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "file.txt"));
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "folder"));
        EXPECT_EQ(read(root_.path() / "target" / "file.txt"), "file");
        EXPECT_EQ(read(root_.path() / "target" / "folder" / "inner" / "inside.txt"), "inside");
        EXPECT_EQ(scannedEntries_, 2u) << "each rename counts as one entry, nothing below is walked";
        ASSERT_FALSE(progress_.empty());
        EXPECT_EQ(progress_.back().entriesDone, 2u);
    }

    TEST_F(LocalCopyOperationTests, AMoveIntoAnExistingDirectoryMergesAndRemovesTheSource)
    {
        write(root_.path() / "folder" / "a.txt", "new a");
        write(root_.path() / "folder" / "sub" / "c.txt", "c");
        write(root_.path() / "target" / "folder" / "b.txt", "old b");

        LocalCopyOperation operation{
            options({entry(root_.path() / "folder", root_.path() / "target" / "folder", true)}, true)
        };
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "folder"));
        EXPECT_EQ(read(root_.path() / "target" / "folder" / "a.txt"), "new a");
        EXPECT_EQ(read(root_.path() / "target" / "folder" / "b.txt"), "old b");
        EXPECT_EQ(read(root_.path() / "target" / "folder" / "sub" / "c.txt"), "c");
    }

    TEST_F(LocalCopyOperationTests, AMoveIntoAnExistingDirectoryRenamesItsItems)
    {
        write(root_.path() / "folder" / "a.txt", "new a");
        write(root_.path() / "target" / "folder" / "b.txt", "old b");
        std::filesystem::create_hard_link(root_.path() / "folder" / "a.txt", root_.path() / "witness.txt");

        LocalCopyOperation operation{
            options({entry(root_.path() / "folder", root_.path() / "target" / "folder", true)}, true)
        };
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "folder"));
        EXPECT_TRUE(std::filesystem::equivalent(root_.path() / "target" / "folder" / "a.txt", root_.path() / "witness.txt"))
            << "the file is renamed, not copied";
    }

    TEST_F(LocalCopyOperationTests, AMergedDirectoryKeepsItsPermissions)
    {
#ifdef _WIN32
        GTEST_SKIP() << "POSIX permissions";
#endif
        using std::filesystem::perms;
        write(root_.path() / "folder" / "a.txt", "a");
        write(root_.path() / "folder" / "sub" / "b.txt", "b");
        write(root_.path() / "target" / "folder" / "c.txt", "c");
        std::filesystem::permissions(root_.path() / "folder", perms::owner_all);
        std::filesystem::permissions(root_.path() / "folder" / "sub", perms::owner_all);
        const auto existing = perms::owner_all | perms::group_read | perms::group_exec;
        std::filesystem::permissions(root_.path() / "target" / "folder", existing);

        LocalCopyOperation operation{
            options({entry(root_.path() / "folder", root_.path() / "target" / "folder", true)}, false)
        };
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(std::filesystem::status(root_.path() / "target" / "folder").permissions(), existing);
        EXPECT_EQ(std::filesystem::status(root_.path() / "target" / "folder" / "sub").permissions(), perms::owner_all)
            << "a created directory takes the permissions of its source";
    }

    TEST_F(LocalCopyOperationTests, AnItemReachedByAnotherPathIsTakenForItself)
    {
        write(root_.path() / "real" / "file.txt", "file");
        if (!makeDirectoryLink(root_.path() / "link", root_.path() / "real"))
            GTEST_SKIP() << "cannot create directory links here";

        LocalCopyOperation operation{
            options({entry(root_.path() / "real" / "file.txt", root_.path() / "link" / "file.txt", true)}, false)
        };
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_EQ(read(root_.path() / "real" / "file.txt"), "file");
        EXPECT_EQ(read(root_.path() / "real" / "file (copy).txt"), "file") << "a copy onto itself takes a free name";
    }

    TEST_F(LocalCopyOperationTests, AHardLinkElsewhereIsAnotherItem)
    {
        write(root_.path() / "a" / "file.txt", "file");
        std::filesystem::create_directories(root_.path() / "b");
        std::filesystem::create_hard_link(root_.path() / "a" / "file.txt", root_.path() / "b" / "file.txt");

        LocalCopyOperation copy{
            options({entry(root_.path() / "a" / "file.txt", root_.path() / "b" / "file.txt", true)}, false)
        };
        ASSERT_TRUE(workToEnd(copy).has_value());
        EXPECT_EQ(countEntries(root_.path() / "a"), 1u) << "no duplicate next to the source";

        LocalCopyOperation move{
            options({entry(root_.path() / "a" / "file.txt", root_.path() / "b" / "file.txt", true)}, true)
        };
        const auto result = workToEnd(move);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(move.getFailed().empty());
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "a" / "file.txt"));
        EXPECT_EQ(read(root_.path() / "b" / "file.txt"), "file");
    }

    TEST_F(LocalCopyOperationTests, AMoveOntoItselfDoesNothing)
    {
        write(root_.path() / "file.txt", "file");

        LocalCopyOperation operation{options({entry(root_.path() / "file.txt", root_.path() / "file.txt")}, true)};
        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_EQ(read(root_.path() / "file.txt"), "file");
        EXPECT_EQ(countEntries(root_.path()), 1u);
    }

    TEST_F(LocalCopyOperationTests, AFileIsCopiedInChunksAcrossSteps)
    {
        const std::string content(10 * 1024 + 7, 'c');
        write(root_.path() / "big.bin", content);

        auto settings = options({entry(root_.path() / "big.bin", root_.path() / "copy.bin")}, false);
        settings.stepBudget = std::chrono::milliseconds{0};
        settings.progressInterval = std::chrono::milliseconds{0};
        settings.chunkSize = 1024;
        LocalCopyOperation operation{std::move(settings)};

        const auto result = workToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(read(root_.path() / "copy.bin"), content);
        const auto midway = std::ranges::find_if(
            progress_,
            [](LocalCopyOperation::Progress const& progress)
            {
                return progress.currentFile.has_value() && progress.fileBytesDone > 0;
            }
        );
        ASSERT_NE(midway, progress_.end()) << "the file shows its own progress while it is copied";
        EXPECT_EQ(midway->fileBytesTotal, content.size());
        EXPECT_EQ(progress_.back().bytesDone, content.size());
        EXPECT_FALSE(progress_.back().currentFile.has_value());
    }

    TEST_F(LocalCopyOperationTests, CancelRemovesThePartialFile)
    {
        write(root_.path() / "big.bin", std::string(64 * 1024, 'c'));
        std::filesystem::create_directories(root_.path() / "target");

        auto settings = options({entry(root_.path() / "big.bin", root_.path() / "target" / "big.bin")}, false);
        settings.stepBudget = std::chrono::milliseconds{0};
        settings.chunkSize = 1024;
        LocalCopyOperation operation{std::move(settings)};

        for (int step = 0; step < 10; ++step)
            ASSERT_EQ(operation.work(), Operation::WorkStatus::MoreWork);
        EXPECT_EQ(countEntries(root_.path() / "target"), 1u) << "the partial file is written";
        ASSERT_TRUE(operation.cancel(true).has_value());
        EXPECT_EQ(countEntries(root_.path() / "target"), 0u);
        EXPECT_EQ(operation.work().error().type, Operation::ErrorType::CannotWorkCanceledOperation);
    }
}
