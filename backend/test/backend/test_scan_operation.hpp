#pragma once

#include <backend/sftp/scan_operation.hpp>
#include "real_server_tests.hpp"

#include <nui/utility/scope_exit.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace Test
{
    class ScanOperationTests : public RealServerTests
    {
      public:
        void SetUp() override
        {
            RealServerTests::SetUp();
        }

      protected:
        static ScanOperation::WorkStatus runScanToCompletion(ScanOperation& operation, int maxIterations = 500)
        {
            ScanOperation::WorkStatus last{};
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                auto workResult = operation.work();
                EXPECT_TRUE(workResult.has_value()) << "work() failed unexpectedly";
                if (!workResult.has_value())
                    return last;
                last = workResult.value();
                if (last == ScanOperation::WorkStatus::Complete)
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
    };

    TEST_F(ScanOperationTests, CanCreateScanOperation)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        ScanOperation operation{*sftp, {}};
    }

    TEST_F(ScanOperationTests, ScanOperationTypeIsScan)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        ScanOperation operation{*sftp, {}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::Scan);
    }

    TEST_F(ScanOperationTests, ScanOperationIsBarrier)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        ScanOperation operation{*sftp, {}};
        EXPECT_TRUE(operation.isBarrier());
    }

    TEST_F(ScanOperationTests, CanScanSingleDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/Documents",
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "doc1.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "doc2.txt"));
    }

    TEST_F(ScanOperationTests, NonRecursiveScanDoesNotDescend)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test",
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        // Root itself plus one-level children only — nothing from inside Documents.
        EXPECT_TRUE(hasEntryNamed(entries, "Documents"));
        EXPECT_TRUE(hasEntryNamed(entries, "file1.txt"));
        EXPECT_FALSE(hasEntryNamed(entries, "doc1.txt"));
        EXPECT_FALSE(hasEntryNamed(entries, "doc2.txt"));
    }

    TEST_F(ScanOperationTests, RecursiveScanFindsNestedEntries)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test",
                .recursive = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "Documents"));
        EXPECT_TRUE(hasEntryNamed(entries, "doc1.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "doc2.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "file1.zip"));
        EXPECT_TRUE(hasEntryNamed(entries, "image1.png"));
        EXPECT_TRUE(hasEntryNamed(entries, "song1.mp3"));
    }

    TEST_F(ScanOperationTests, ScanReportsCorrectFileSizeAndType)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test",
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();

        auto const* fileEntry = findEntryNamed(entries, "file1.txt");
        ASSERT_NE(fileEntry, nullptr);
        EXPECT_EQ(fileEntry->type, SharedData::FileType::Regular);
        EXPECT_EQ(fileEntry->size, std::string{"Fake file content"}.size());

        auto const* largeEntry = findEntryNamed(entries, "large.txt");
        ASSERT_NE(largeEntry, nullptr);
        EXPECT_EQ(largeEntry->type, SharedData::FileType::Regular);
        EXPECT_EQ(largeEntry->size, 65536u);

        auto const* dirEntry = findEntryNamed(entries, "Documents");
        ASSERT_NE(dirEntry, nullptr);
        EXPECT_EQ(dirEntry->type, SharedData::FileType::Directory);

        auto const* linkEntry = findEntryNamed(entries, "link_to_file1.txt");
        ASSERT_NE(linkEntry, nullptr);
        EXPECT_EQ(linkEntry->type, SharedData::FileType::Symlink);
    }

    TEST_F(ScanOperationTests, TotalBytesMatchesSumOfRegularFiles)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/Documents",
                .recursive = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);

        const auto reportedBytes = operation.totalBytes();
        const auto expectedBytes =
            std::string{"Document 1 content"}.size() + std::string{"Document 2 content"}.size();
        EXPECT_EQ(reportedBytes, expectedBytes);
    }

    TEST_F(ScanOperationTests, ProgressCallbackInvokedAtLeastOnce)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        int callbackInvocations = 0;
        ScanOperation operation{
            *sftp,
            {
                .progressCallback =
                    [&callbackInvocations](std::uint64_t, std::uint64_t, std::uint64_t) {
                        ++callbackInvocations;
                    },
                .remotePath = "/home/test/Documents",
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);
        EXPECT_GT(callbackInvocations, 0);
    }

    TEST_F(ScanOperationTests, ScanNonExistentRemoteDirectoryFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/does_not_exist_dir",
                .recursive = false,
            }};

        std::expected<ScanOperation::WorkStatus, ScanOperation::Error> workResult{ScanOperation::WorkStatus::MoreWork};
        for (int iteration = 0; iteration < 100; ++iteration)
        {
            workResult = operation.work();
            if (!workResult.has_value())
                break;
            if (workResult.value() == ScanOperation::WorkStatus::Complete)
                break;
        }

        ASSERT_FALSE(workResult.has_value()) << "scan of non-existent directory must fail";
        EXPECT_EQ(workResult.error().type, ScanOperation::ErrorType::SftpError);
    }

    TEST_F(ScanOperationTests, ScanOfRegularFilePathFails)
    {
        // Attempting to scan a regular file path (not a directory) must surface as an error
        // rather than crash or silently succeed.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .recursive = false,
            }};

        std::expected<ScanOperation::WorkStatus, ScanOperation::Error> workResult{ScanOperation::WorkStatus::MoreWork};
        for (int iteration = 0; iteration < 100; ++iteration)
        {
            workResult = operation.work();
            if (!workResult.has_value())
                break;
            if (workResult.value() == ScanOperation::WorkStatus::Complete)
                break;
        }

        ASSERT_FALSE(workResult.has_value()) << "scan of a regular file must fail";
        EXPECT_EQ(workResult.error().type, ScanOperation::ErrorType::SftpError);
    }

    TEST_F(ScanOperationTests, WorkOnCompletedScanReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/Documents",
                .recursive = false,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);

        auto secondCallResult = operation.work();
        ASSERT_FALSE(secondCallResult.has_value());
        EXPECT_EQ(secondCallResult.error().type, ScanOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(ScanOperationTests, WorkOnFailedScanReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/does_not_exist_dir",
                .recursive = false,
            }};

        // Drive the state machine until it fails.
        for (int iteration = 0; iteration < 10; ++iteration)
        {
            auto result = operation.work();
            if (!result.has_value())
                break;
        }

        auto secondAttempt = operation.work();
        ASSERT_FALSE(secondAttempt.has_value());
        EXPECT_EQ(secondAttempt.error().type, ScanOperation::ErrorType::CannotWorkFailedOperation);
    }

    TEST_F(ScanOperationTests, CancelledScanCannotBeWorkedFurther)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test",
                .recursive = true,
            }};

        // Kick into Running state.
        auto firstWork = operation.work();
        ASSERT_TRUE(firstWork.has_value());

        ASSERT_TRUE(operation.cancel(true).has_value());

        auto afterCancel = operation.work();
        ASSERT_FALSE(afterCancel.has_value());
        EXPECT_EQ(afterCancel.error().type, ScanOperation::ErrorType::CannotWorkCanceledOperation);
    }

    TEST_F(ScanOperationTests, IgnoreHiddenOptionDoesNotBreakScanOfNonHiddenTree)
    {
        // The fake server has no dotfiles, so we just make sure the option doesn't regress the
        // normal scan results — every expected entry is still present.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ScanOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/Documents",
                .recursive = false,
                .ignoreHidden = true,
            }};

        ASSERT_EQ(runScanToCompletion(operation), ScanOperation::WorkStatus::Complete);

        auto entries = operation.ejectEntries();
        EXPECT_TRUE(hasEntryNamed(entries, "doc1.txt"));
        EXPECT_TRUE(hasEntryNamed(entries, "doc2.txt"));
    }
}
