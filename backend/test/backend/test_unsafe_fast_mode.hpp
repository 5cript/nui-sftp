#pragma once

#include <backend/sftp/unsafe_fast_mode.hpp>
#include <backend/sftp/upload_operation.hpp>
#include "test_transfer_failures.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>

namespace Test
{
    class UnsafeFastModeTests : public TransferFailureTests
    {
      public:
        static UnsafeFastMode always(bool unsafe)
        {
            return [unsafe]()
            {
                return unsafe;
            };
        }

        /**
         * @brief How often the mock answered @p type since the last reset, 0 when it never did.
         */
        static int requests(nlohmann::json const& counts, std::string const& type)
        {
            return counts.value(type, 0);
        }

        /**
         * @brief The renames of a move into place, plain or as posix-rename@openssh.com.
         */
        static int renames(nlohmann::json const& counts)
        {
            return requests(counts, "RENAME") + requests(counts, "EXTENDED");
        }

        UploadOperation::UploadOperationOptions uploadOf(std::string const& name, UnsafeFastMode mode) const
        {
            return {
                .remotePath = remote("/home/test/" + name),
                .localPath = toLocal(localDirectory_.path() / name),
                .unsafeFastMode = std::move(mode),
            };
        }
    };

    TEST_F(UnsafeFastModeTests, AnUploadWritesTheDestinationWithoutTheRoundTripsOfTheSafeMode)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        writeLocalFile(localDirectory_.path() / "safe.txt", "safe content");
        writeLocalFile(localDirectory_.path() / "fast.txt", "fast content");

        ASSERT_TRUE(serverStartResult->command("resetRequestCounts", {}));
        {
            UploadOperation operation{sftp, uploadOf("safe.txt", always(false))};
            ASSERT_TRUE(runToEnd(operation).has_value());
        }
        const auto safe = serverStartResult->query("requestCounts");
        ASSERT_TRUE(safe.has_value());

        ASSERT_TRUE(serverStartResult->command("resetRequestCounts", {}));
        {
            UploadOperation operation{sftp, uploadOf("fast.txt", always(true))};
            ASSERT_TRUE(runToEnd(operation).has_value());
        }
        const auto fast = serverStartResult->query("requestCounts");
        ASSERT_TRUE(fast.has_value());

        EXPECT_GE(requests(*safe, "LSTAT"), 2) << safe->dump();
        EXPECT_GE(renames(*safe), 1) << safe->dump();
        EXPECT_EQ(requests(*fast, "LSTAT"), 0) << fast->dump();
        EXPECT_EQ(renames(*fast), 0) << fast->dump();
        EXPECT_EQ(requests(*fast, "OPEN"), 1) << fast->dump();
        EXPECT_EQ(requests(*fast, "REMOVE"), 0) << fast->dump();

        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/safe.txt"), "safe content");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/fast.txt"), "fast content");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/fast.txt.filepart"), "<missing>");
    }

    TEST_F(UnsafeFastModeTests, AnUploadOverwritesADestinationThatMayBeOverwritten)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        writeLocalFile(localDirectory_.path() / "file1.txt", "new");

        auto options = uploadOf("file1.txt", always(true));
        options.mayOverwrite = true;
        UploadOperation operation{sftp, std::move(options)};
        ASSERT_TRUE(runToEnd(operation).has_value());

        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "new");
    }

    TEST_F(UnsafeFastModeTests, AnUploadLeavesADestinationThatMayNotBeOverwritten)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        writeLocalFile(localDirectory_.path() / "file1.txt", "new");

        UploadOperation operation{sftp, uploadOf("file1.txt", always(true))};
        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::FileExists);
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "Fake file content");
    }

    TEST_F(UnsafeFastModeTests, AFailedUploadRemovesItsPartialDestination)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        writeLocalFile(localDirectory_.path() / "partial.txt", alphabet(64 * 1024));
        ASSERT_TRUE(serverStartResult->command("failWritesAfter", {{"bytes", 1024}}));

        UploadOperation operation{sftp, uploadOf("partial.txt", always(true))};
        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/partial.txt"), "<missing>");
    }

    TEST_F(UnsafeFastModeTests, AStoppedUploadRemovesItsPartialDestination)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        writeLocalFile(localDirectory_.path() / "stopped.txt", alphabet(512 * 1024));

        auto options = uploadOf("stopped.txt", always(true));
        options.bigFileOptimized = true;
        UploadOperation operation{sftp, std::move(options)};
        const auto started = operation.work();
        ASSERT_TRUE(started.has_value());
        ASSERT_NE(started.value(), Operation::WorkStatus::Complete);
        ASSERT_TRUE(operation.cancel(true).has_value());

        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/stopped.txt"), "<missing>");
    }

    TEST_F(UnsafeFastModeTests, AnUploadAtTheSizeLimitKeepsItsTempFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        writeLocalFile(localDirectory_.path() / "below.txt", alphabet(unsafeFastSizeLimit - 1));
        writeLocalFile(localDirectory_.path() / "limit.txt", alphabet(unsafeFastSizeLimit));

        UploadOperation below{sftp, uploadOf("below.txt", always(true))};
        ASSERT_TRUE(below.prepare().has_value());
        UploadOperation limit{sftp, uploadOf("limit.txt", always(true))};
        ASSERT_TRUE(limit.prepare().has_value());

        EXPECT_NE(readRemoteFile(*sftp, "/home/test/below.txt"), "<missing>");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/below.txt.filepart"), "<missing>");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/limit.txt"), "<missing>");
        EXPECT_NE(readRemoteFile(*sftp, "/home/test/limit.txt.filepart"), "<missing>");
    }

    TEST_F(UnsafeFastModeTests, AnUploadKeepsTheModeItOpenedItsFileWith)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        const auto content = alphabet(256 * 1024);
        writeLocalFile(localDirectory_.path() / "switched.txt", content);
        const auto unsafe = std::make_shared<std::atomic_bool>(true);

        auto options = uploadOf(
            "switched.txt",
            [unsafe]()
            {
                return unsafe->load();
            }
        );
        options.bigFileOptimized = true;
        UploadOperation operation{sftp, std::move(options)};
        ASSERT_TRUE(serverStartResult->command("resetRequestCounts", {}));
        const auto started = operation.work();
        ASSERT_TRUE(started.has_value());
        ASSERT_NE(started.value(), Operation::WorkStatus::Complete);
        unsafe->store(false);
        ASSERT_TRUE(runToEnd(operation).has_value());
        const auto counts = serverStartResult->query("requestCounts");
        ASSERT_TRUE(counts.has_value());

        EXPECT_EQ(renames(*counts), 0) << counts->dump();
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/switched.txt"), content);
    }
}
