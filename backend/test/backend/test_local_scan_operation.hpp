#pragma once

#include <backend/sftp/local_scan_operation.hpp>
#include "real_server_tests.hpp"

#include <utility/temporary_directory.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace Test
{
    class LocalScanOperationTests : public ::testing::Test
    {
      public:
        void SetUp() override
        {
            writeFile(scanRoot_.path() / "file1.txt", "Local file content");
            writeFile(scanRoot_.path() / "notes.md", "hello");
            std::filesystem::create_directory(scanRoot_.path() / "subdir");
            writeFile(scanRoot_.path() / "subdir" / "nested.txt", "nested content");
            std::filesystem::create_directory(scanRoot_.path() / "subdir" / "deeper");
            writeFile(scanRoot_.path() / "subdir" / "deeper" / "leaf.txt", "leaf");

            // Hidden files — used by the ignoreHidden tests.
            writeFile(scanRoot_.path() / ".hidden_file", "secret");
            std::filesystem::create_directory(scanRoot_.path() / ".hidden_dir");
            writeFile(scanRoot_.path() / ".hidden_dir" / "child.txt", "child");
        }

      protected:
        static void writeFile(std::filesystem::path const& path, std::string_view contents)
        {
            std::ofstream stream{path, std::ios::binary};
            stream << contents;
        }

        static LocalScanOperation::WorkStatus
        runScanToCompletion(LocalScanOperation& operation, int maxIterations = 500)
        {
            LocalScanOperation::WorkStatus last{};
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                auto workResult = operation.work();
                EXPECT_TRUE(workResult.has_value()) << "work() failed unexpectedly";
                if (!workResult.has_value())
                    return last;
                last = workResult.value();
                if (last == LocalScanOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }

        static bool hasEntryNamed(
            std::vector<SharedData::DirectoryEntry> const& entries,
            std::string const& filename)
        {
            return std::any_of(entries.begin(), entries.end(), [&](SharedData::DirectoryEntry const& entry) {
                return entry.path.filename().generic_string() == filename;
            });
        }

        static SharedData::DirectoryEntry const* findEntryNamed(
            std::vector<SharedData::DirectoryEntry> const& entries,
            std::string const& filename)
        {
            auto iter = std::find_if(entries.begin(), entries.end(), [&](SharedData::DirectoryEntry const& entry) {
                return entry.path.filename().generic_string() == filename;
            });
            return iter == entries.end() ? nullptr : &*iter;
        }

      protected:
        Utility::TemporaryDirectory scanRoot_{programDirectory / "temp", true};
    };

    TEST_F(LocalScanOperationTests, CanCreateLocalScanOperation)
    {
        LocalScanOperation operation{{.localPath = scanRoot_.path()}};
    }

    TEST_F(LocalScanOperationTests, LocalScanOperationTypeIsLocalScan)
    {
        LocalScanOperation operation{{.localPath = scanRoot_.path()}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::LocalScan);
    }

    TEST_F(LocalScanOperationTests, LocalScanOperationIsBarrier)
    {
        LocalScanOperation operation{{.localPath = scanRoot_.path()}};
        EXPECT_TRUE(operation.isBarrier());
    }

    TEST_F(LocalScanOperationTests, CanScanSingleDirectory)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "notes.md"));
        EXPECT_TRUE(hasEntryNamed(entries, "subdir"));
    }

    TEST_F(LocalScanOperationTests, NonRecursiveScanDoesNotDescend)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "subdir"));
        EXPECT_FALSE(hasEntryNamed(entries, "nested.txt"));
        EXPECT_FALSE(hasEntryNamed(entries, "leaf.txt"));
    }

    TEST_F(LocalScanOperationTests, RecursiveScanFindsNestedEntries)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "subdir"));
        EXPECT_TRUE(hasEntryNamed(entries, "nested.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "deeper"));
        EXPECT_TRUE(hasEntryNamed(entries, "leaf.txt"));
    }

    TEST_F(LocalScanOperationTests, ScanReportsCorrectFileSizeAndType)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();

        auto const* fileEntry = findEntryNamed(entries, "file1.txt");
        ASSERT_NE(fileEntry, nullptr);
        EXPECT_EQ(fileEntry->type, SharedData::FileType::Regular);
        EXPECT_EQ(fileEntry->size, std::string{"Local file content"}.size());

        auto const* dirEntry = findEntryNamed(entries, "subdir");
        ASSERT_NE(dirEntry, nullptr);
        EXPECT_EQ(dirEntry->type, SharedData::FileType::Directory);
        EXPECT_EQ(dirEntry->size, 0u);
    }

    TEST_F(LocalScanOperationTests, TotalBytesMatchesSumOfRegularFiles)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path() / "subdir",
                .recursive = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        const auto reportedBytes = operation.totalBytes();
        const auto expectedBytes =
            std::string{"nested content"}.size() + std::string{"leaf"}.size();
        EXPECT_EQ(reportedBytes, expectedBytes);
    }

    TEST_F(LocalScanOperationTests, ProgressCallbackInvokedAtLeastOnce)
    {
        int callbackInvocations = 0;
        LocalScanOperation operation{
            {
                .progressCallback =
                    [&callbackInvocations](std::uint64_t, std::uint64_t, std::uint64_t) {
                        ++callbackInvocations;
                    },
                .localPath = scanRoot_.path(),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);
        EXPECT_GT(callbackInvocations, 0);
    }

    TEST_F(LocalScanOperationTests, IgnoreHiddenSkipsDotfiles)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = false,
                .ignoreHidden = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));
        EXPECT_FALSE(hasEntryNamed(entries, ".hidden_file"));
        EXPECT_FALSE(hasEntryNamed(entries, ".hidden_dir"));
    }

    TEST_F(LocalScanOperationTests, WithoutIgnoreHiddenDotfilesArePresent)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = false,
                .ignoreHidden = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, ".hidden_file"));
        EXPECT_TRUE(hasEntryNamed(entries, ".hidden_dir"));
    }

    TEST_F(LocalScanOperationTests, ScanNonExistentLocalDirectoryFailsGracefully)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path() / "does_not_exist",
                .recursive = false,
            }};

        // We want a graceful error result, not an uncaught filesystem exception.
        std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error> workResult{
            LocalScanOperation::WorkStatus::MoreWork};
        bool threw = false;
        try
        {
            for (int iteration = 0; iteration < 100; ++iteration)
            {
                workResult = operation.work();
                if (!workResult.has_value())
                    break;
                if (workResult.value() == LocalScanOperation::WorkStatus::Complete)
                    break;
            }
        }
        catch (...)
        {
            threw = true;
        }

        EXPECT_FALSE(threw) << "scan of a non-existent directory must not throw";
        ASSERT_FALSE(workResult.has_value()) << "scan of a non-existent directory must return an error";
        EXPECT_EQ(workResult.error().type, LocalScanOperation::ErrorType::FilesystemError);
    }

    TEST_F(LocalScanOperationTests, ScanOfRegularFilePathFailsGracefully)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path() / "file1.txt",
                .recursive = false,
            }};

        std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error> workResult{
            LocalScanOperation::WorkStatus::MoreWork};
        bool threw = false;
        try
        {
            for (int iteration = 0; iteration < 100; ++iteration)
            {
                workResult = operation.work();
                if (!workResult.has_value())
                    break;
                if (workResult.value() == LocalScanOperation::WorkStatus::Complete)
                    break;
            }
        }
        catch (...)
        {
            threw = true;
        }

        EXPECT_FALSE(threw) << "scan of a regular file path must not throw";
        ASSERT_FALSE(workResult.has_value()) << "scan of a regular file path must return an error";
        EXPECT_EQ(workResult.error().type, LocalScanOperation::ErrorType::FilesystemError);
    }

    TEST_F(LocalScanOperationTests, WorkOnCompletedScanReturnsError)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), LocalScanOperation::WorkStatus::Complete);

        auto secondCallResult = operation.work();
        ASSERT_FALSE(secondCallResult.has_value());
        EXPECT_EQ(secondCallResult.error().type, LocalScanOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(LocalScanOperationTests, WorkOnFailedScanReturnsError)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path() / "does_not_exist",
                .recursive = false,
            }};

        try
        {
            for (int iteration = 0; iteration < 10; ++iteration)
            {
                auto result = operation.work();
                if (!result.has_value())
                    break;
            }
        }
        catch (...)
        {
            // Swallow here so the "work-after-failure" probe below still runs.
        }

        auto secondAttempt = operation.work();
        ASSERT_FALSE(secondAttempt.has_value());
        EXPECT_EQ(secondAttempt.error().type, LocalScanOperation::ErrorType::CannotWorkFailedOperation);
    }

    TEST_F(LocalScanOperationTests, CancelledScanCannotBeWorkedFurther)
    {
        LocalScanOperation operation{
            {
                .localPath = scanRoot_.path(),
                .recursive = true,
            }};

        auto firstWork = operation.work();
        ASSERT_TRUE(firstWork.has_value());

        ASSERT_TRUE(operation.cancel(true).has_value());

        auto afterCancel = operation.work();
        ASSERT_FALSE(afterCancel.has_value());
        EXPECT_EQ(afterCancel.error().type, LocalScanOperation::ErrorType::CannotWorkCanceledOperation);
    }

    TEST_F(LocalScanOperationTests, ScanStrandIsNullForLocalScan)
    {
        LocalScanOperation operation{{.localPath = scanRoot_.path()}};
        EXPECT_EQ(operation.strand(), nullptr);
    }
}
