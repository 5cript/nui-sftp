#pragma once

#include <backend/sftp/download_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include "operation_runs.hpp"
#include "remote_protocol_tests.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Test
{
    /**
     * @brief Single downloads and uploads on both protocols.
     */
    class ProtocolTransferTests : public RemoteProtocolTests
    {
      protected:
        static std::string partOf(std::filesystem::path const& path)
        {
            return fmt::format("{}.filepart", path.string());
        }

        DownloadOperation::DownloadOperationOptions
        downloadOf(std::string_view relative, std::filesystem::path const& local, bool bigFileOptimized = false) const
        {
            return {
                .remotePath = remoteAt(relative),
                .localPath = toLocal(local),
                .bigFileOptimized = bigFileOptimized,
            };
        }

        /**
         * @brief An upload of the local file @p name, written with @p content, to @p name below the work directory.
         */
        UploadOperation::UploadOperationOptions
        uploadOf(std::string_view name, std::string_view content, bool bigFileOptimized = false) const
        {
            const auto local = localDirectory_.path() / "source" / name;
            writeLocalFile(local, content);
            return {
                .remotePath = remoteAt(name),
                .localPath = toLocal(local),
                .bigFileOptimized = bigFileOptimized,
            };
        }

        static UnsafeFastMode unsafeFast()
        {
            return []()
            {
                return true;
            };
        }
    };

    TEST_P(ProtocolTransferTests, ADownloadWritesTheRemoteFile)
    {
        const auto content = patternedContent(300u * 1024u);
        putRemoteFile("file.bin", content);
        for (const bool bigFileOptimized : {false, true})
        {
            SCOPED_TRACE(fmt::format("bigFileOptimized: {}", bigFileOptimized));
            const auto local = localDirectory_.path() / fmt::format("file{}.bin", bigFileOptimized);
            DownloadOperation operation{fileSystem(), downloadOf("file.bin", local, bigFileOptimized)};

            const auto result = runToEnd(operation, operationLimit);

            ASSERT_TRUE(result.has_value()) << result.error().toString();
            EXPECT_EQ(localContentOf(local), content);
            EXPECT_FALSE(std::filesystem::exists(partOf(local)));
        }
    }

    TEST_P(ProtocolTransferTests, ADownloadOfAnEmptyFileWritesAnEmptyFile)
    {
        putRemoteFile("empty.txt", "");
        const auto local = localDirectory_.path() / "empty.txt";
        DownloadOperation operation{fileSystem(), downloadOf("empty.txt", local)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(localContentOf(local), "");
    }

    TEST_P(ProtocolTransferTests, ADownloadLeavesAnExistingLocalFile)
    {
        putRemoteFile("file.txt", "remote content");
        const auto local = localDirectory_.path() / "file.txt";
        writeLocalFile(local, "local content");
        DownloadOperation operation{fileSystem(), downloadOf("file.txt", local)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::FileExists);
        EXPECT_EQ(localContentOf(local), "local content");
    }

    TEST_P(ProtocolTransferTests, ADownloadReplacesAnExistingLocalFileWhenAllowed)
    {
        putRemoteFile("file.txt", "remote content");
        const auto local = localDirectory_.path() / "file.txt";
        writeLocalFile(local, "local content that is longer");
        auto options = downloadOf("file.txt", local);
        options.mayOverwrite = true;
        DownloadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(localContentOf(local), "remote content");
    }

    TEST_P(ProtocolTransferTests, ADownloadContinuesThePartialFileOfItsSource)
    {
        const auto content = patternedContent(1024u * 1024u);
        constexpr std::size_t partial = 600u * 1024u;
        putRemoteFile("file.bin", content);
        const auto local = localDirectory_.path() / "file.bin";
        writeLocalFile(partOf(local), std::string_view{content}.substr(0u, partial));
        std::optional<std::uint64_t> firstProgress{};
        auto options = downloadOf("file.bin", local);
        options.progressCallback = [&firstProgress](auto, auto, auto current, auto)
        {
            if (!firstProgress && current > 0u)
                firstProgress = static_cast<std::uint64_t>(current);
        };
        options.tryContinue = true;
        DownloadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(localContentOf(local), content);
        ASSERT_TRUE(firstProgress.has_value());
        EXPECT_GT(*firstProgress, partial);
    }

    TEST_P(ProtocolTransferTests, ADownloadStartsOverThePartialFileOfAnotherSource)
    {
        const auto content = patternedContent(64u * 1024u);
        putRemoteFile("file.bin", content);
        const auto local = localDirectory_.path() / "file.bin";
        writeLocalFile(partOf(local), std::string(16u * 1024u, 'w'));
        auto options = downloadOf("file.bin", local);
        options.tryContinue = true;
        DownloadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(localContentOf(local), content);
    }

    TEST_P(ProtocolTransferTests, ADownloadGivesItsFileTheModificationTimeOfItsSource)
    {
        // 2023-11-14 22:13:57 UTC, seconds that a listing to the minute or the day leaves out.
        constexpr std::uint64_t sourceTime{1700000037u};
        putRemoteFile("timed.txt", "timed content");
        ASSERT_NO_FATAL_FAILURE(setRemoteModificationTime("timed.txt", sourceTime));
        for (const bool inheritPermissions : {true, false})
        {
            SCOPED_TRACE(fmt::format("inheritPermissions: {}", inheritPermissions));
            const auto local = localDirectory_.path() / fmt::format("timed{}.txt", inheritPermissions);
            auto options = downloadOf("timed.txt", local);
            options.inheritPermissions = inheritPermissions;
            DownloadOperation operation{fileSystem(), std::move(options)};

            const auto result = runToEnd(operation, operationLimit);

            ASSERT_TRUE(result.has_value()) << result.error().toString();
            EXPECT_EQ(localModificationTimeOf(local), sourceTime);
        }
    }

    TEST_P(ProtocolTransferTests, AStoppedDownloadEndsCanceledAndTheNextOneRuns)
    {
        putRemoteFile("big.bin", patternedContent(4u * 1024u * 1024u));
        putRemoteFile("small.txt", "small content");
        const auto local = localDirectory_.path() / "big.bin";
        std::uint64_t received{0u};
        auto options = downloadOf("big.bin", local);
        options.progressCallback = [&received](auto, auto, auto current, auto)
        {
            received = std::max(received, static_cast<std::uint64_t>(current));
        };
        options.doCleanup = true;
        DownloadOperation operation{fileSystem(), std::move(options)};
        ASSERT_TRUE(workUntil(
            operation,
            [&received]()
            {
                return received > 0u;
            }
        ));

        ASSERT_TRUE(operation.cancel(true).has_value());

        EXPECT_EQ(operation.state(), SharedData::OperationState::Canceled);
        EXPECT_FALSE(std::filesystem::exists(local));
        EXPECT_FALSE(std::filesystem::exists(partOf(local)));
        const auto next = localDirectory_.path() / "small.txt";
        DownloadOperation nextOperation{fileSystem(), downloadOf("small.txt", next)};
        const auto result = runToEnd(nextOperation, operationLimit);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(localContentOf(next), "small content");
    }

    TEST_P(ProtocolTransferTests, AStoppedAsynchronousDownloadEndsCanceledAndTheNextOneRuns)
    {
        putRemoteFile("big.bin", patternedContent(4u * 1024u * 1024u));
        putRemoteFile("small.txt", "small content");
        stallFtpDownloadsAfter(1024u * 1024u);
        const auto local = localDirectory_.path() / "big.bin";
        std::uint64_t received{0u};
        auto options = downloadOf("big.bin", local, true);
        options.progressCallback = [&received](auto, auto, auto current, auto)
        {
            received = std::max(received, static_cast<std::uint64_t>(current));
        };
        options.doCleanup = true;
        DownloadOperation operation{fileSystem(), std::move(options)};
        ASSERT_TRUE(workUntil(
            operation,
            [&received]()
            {
                return received > 0u;
            }
        ));

        ASSERT_TRUE(operation.cancel(true).has_value());

        EXPECT_EQ(operation.state(), SharedData::OperationState::Canceled);
        EXPECT_FALSE(std::filesystem::exists(local));
        EXPECT_FALSE(std::filesystem::exists(partOf(local)));
        stallFtpDownloadsAfter(std::nullopt);
        const auto next = localDirectory_.path() / "small.txt";
        DownloadOperation nextOperation{fileSystem(), downloadOf("small.txt", next, true)};
        const auto result = runToEnd(nextOperation, operationLimit);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(localContentOf(next), "small content");
    }

    TEST_P(ProtocolTransferTests, AnUploadWritesTheLocalFile)
    {
        const auto content = patternedContent(300u * 1024u);
        for (const bool bigFileOptimized : {false, true})
        {
            SCOPED_TRACE(fmt::format("bigFileOptimized: {}", bigFileOptimized));
            const auto name = fmt::format("file{}.bin", bigFileOptimized);
            UploadOperation operation{fileSystem(), uploadOf(name, content, bigFileOptimized)};

            const auto result = runToEnd(operation, operationLimit);

            ASSERT_TRUE(result.has_value()) << result.error().toString();
            EXPECT_EQ(remoteContentOf(name), content);
            EXPECT_FALSE(remoteExists(fmt::format("{}.filepart", name)));
        }
    }

    TEST_P(ProtocolTransferTests, AnUploadOfAnEmptyFileWritesAnEmptyFile)
    {
        UploadOperation operation{fileSystem(), uploadOf("empty.txt", "")};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("empty.txt"), "");
    }

    TEST_P(ProtocolTransferTests, AnUploadLeavesAnExistingRemoteFile)
    {
        // A small file is checked at the rename, a larger one before it is sent too.
        for (const std::size_t size : {std::size_t{16u}, std::size_t{8u * 1024u}})
        {
            SCOPED_TRACE(fmt::format("size: {}", size));
            const auto name = fmt::format("kept{}.txt", size);
            putRemoteFile(name, "kept content");
            UploadOperation operation{fileSystem(), uploadOf(name, std::string(size, 'n'))};

            const auto result = runToEnd(operation, operationLimit);

            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().type, Operation::ErrorType::FileExists);
            EXPECT_EQ(remoteContentOf(name), "kept content");
            EXPECT_FALSE(remoteExists(fmt::format("{}.filepart", name)));
        }
    }

    TEST_P(ProtocolTransferTests, AnUploadReplacesAnExistingRemoteFileWhenAllowed)
    {
        putRemoteFile("replaced.txt", "old content that is longer");
        auto options = uploadOf("replaced.txt", "new content");
        options.mayOverwrite = true;
        UploadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("replaced.txt"), "new content");
        EXPECT_FALSE(remoteExists("replaced.txt.filepart"));
    }

    TEST_P(ProtocolTransferTests, AnUploadKeepsTheModificationTimeOfItsSource)
    {
        ASSERT_TRUE(fileSystem()->capabilities().setModificationTime);
        // 2023-11-14 22:13:57 UTC.
        constexpr std::uint64_t sourceTime{1700000037u};
        auto options = uploadOf("timed.txt", "timed content");
        std::filesystem::last_write_time(
            localDirectory_.path() / "source" / "timed.txt",
            std::chrono::file_clock::from_sys(std::chrono::sys_seconds{std::chrono::seconds{sourceTime}})
        );
        UploadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        const auto uploaded = fileSystem()->later(RemoteIo::Stat{.path = remoteAt("timed.txt")}).get();
        ASSERT_TRUE(uploaded.has_value()) << uploaded.error().toString();
        EXPECT_EQ(uploaded->mtime, sourceTime);
    }

    TEST_P(ProtocolTransferTests, AnUploadContinuesItsPartialFile)
    {
        const auto content = patternedContent(1024u * 1024u);
        constexpr std::size_t partial = 600u * 1024u;
        putRemoteFile("file.bin.filepart", content.substr(0u, partial));
        std::optional<std::uint64_t> firstProgress{};
        auto options = uploadOf("file.bin", content);
        options.progressCallback = [&firstProgress](auto, auto, auto current, auto)
        {
            if (!firstProgress && current > 0u)
                firstProgress = static_cast<std::uint64_t>(current);
        };
        options.tryContinue = true;
        UploadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("file.bin"), content);
        EXPECT_FALSE(remoteExists("file.bin.filepart"));
        ASSERT_TRUE(firstProgress.has_value());
        EXPECT_GT(*firstProgress, partial);
    }

    TEST_P(ProtocolTransferTests, AnUploadStartsOverThePartialFileOfAnotherSource)
    {
        const auto content = patternedContent(64u * 1024u);
        putRemoteFile("file.bin.filepart", std::string(16u * 1024u, 'w'));
        auto options = uploadOf("file.bin", content);
        options.tryContinue = true;
        UploadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("file.bin"), content);
    }

    TEST_P(ProtocolTransferTests, AStoppedUploadKeepsAPartialFileThatTheNextUploadContinues)
    {
        const auto content = patternedContent(4u * 1024u * 1024u);
        {
            std::uint64_t sent{0u};
            auto options = uploadOf("file.bin", content);
            options.progressCallback = [&sent](auto, auto, auto current, auto)
            {
                sent = static_cast<std::uint64_t>(current);
            };
            UploadOperation stopped{fileSystem(), std::move(options)};
            ASSERT_TRUE(workUntil(
                stopped,
                [&sent]()
                {
                    return sent > 0u;
                }
            ));
            stopped.partialFileOnCancel(UploadOperation::PartialFile::Kept);
            ASSERT_TRUE(stopped.cancel(true).has_value());
            EXPECT_EQ(stopped.state(), SharedData::OperationState::Canceled);
        }
        // What was on the way when the transfer stopped may be lost, the rest stays.
        const auto kept = remoteContentOf("file.bin.filepart");
        ASSERT_TRUE(kept.has_value());
        EXPECT_LT(kept->size(), content.size());
        EXPECT_TRUE(content.starts_with(*kept));
        EXPECT_FALSE(remoteExists("file.bin"));

        std::optional<std::uint64_t> firstProgress{};
        auto options = uploadOf("file.bin", content);
        options.progressCallback = [&firstProgress](auto, auto, auto current, auto)
        {
            if (!firstProgress && current > 0u)
                firstProgress = static_cast<std::uint64_t>(current);
        };
        UploadOperation continued{fileSystem(), std::move(options)};
        const auto result = runToEnd(continued, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("file.bin"), content);
        EXPECT_FALSE(remoteExists("file.bin.filepart"));
        ASSERT_TRUE(firstProgress.has_value());
        EXPECT_GT(*firstProgress, kept->size());
    }

    TEST_P(ProtocolTransferTests, AStoppedUploadDiscardingItsPartialFileLeavesNone)
    {
        std::uint64_t sent{0u};
        auto options = uploadOf("file.bin", patternedContent(4u * 1024u * 1024u));
        options.progressCallback = [&sent](auto, auto, auto current, auto)
        {
            sent = static_cast<std::uint64_t>(current);
        };
        UploadOperation operation{fileSystem(), std::move(options)};
        ASSERT_TRUE(workUntil(
            operation,
            [&sent]()
            {
                return sent > 0u;
            }
        ));

        operation.partialFileOnCancel(UploadOperation::PartialFile::Discarded);
        ASSERT_TRUE(operation.cancel(true).has_value());

        EXPECT_EQ(operation.state(), SharedData::OperationState::Canceled);
        EXPECT_FALSE(remoteExists("file.bin.filepart"));
        EXPECT_FALSE(remoteExists("file.bin"));
    }

    TEST_P(ProtocolTransferTests, AnUnsafeFastUploadWritesStraightToItsDestination)
    {
        auto options = uploadOf("fast.txt", "fast content");
        options.unsafeFastMode = unsafeFast();
        UploadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("fast.txt"), "fast content");
        if (auto const* tree = servedTree())
            EXPECT_EQ(tree->stored(), (std::vector<std::string>{remoteAt("fast.txt").value()}));
    }

    TEST_P(ProtocolTransferTests, AnUnsafeFastUploadLeavesAnExistingRemoteFileUnlessAllowed)
    {
        putRemoteFile("kept.txt", "kept content");
        putRemoteFile("replaced.txt", "old content");
        auto kept = uploadOf("kept.txt", "new content");
        kept.unsafeFastMode = unsafeFast();
        auto replaced = uploadOf("replaced.txt", "new content");
        replaced.unsafeFastMode = unsafeFast();
        replaced.mayOverwrite = true;
        UploadOperation keeping{fileSystem(), std::move(kept)};
        UploadOperation replacing{fileSystem(), std::move(replaced)};

        const auto refused = runToEnd(keeping, operationLimit);
        const auto result = runToEnd(replacing, operationLimit);

        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().type, Operation::ErrorType::FileExists);
        EXPECT_EQ(remoteContentOf("kept.txt"), "kept content");
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(remoteContentOf("replaced.txt"), "new content");
    }

    TEST_P(ProtocolTransferTests, AnUnsafeFastDownloadWritesStraightToItsDestination)
    {
        putRemoteFile("fast.txt", "fast content");
        const auto local = localDirectory_.path() / "fast.txt";
        auto options = downloadOf("fast.txt", local);
        options.unsafeFastMode = unsafeFast();
        DownloadOperation operation{fileSystem(), std::move(options)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(localContentOf(local), "fast content");
    }

    INSTANTIATE_FOR_BOTH_PROTOCOLS(ProtocolTransferTests);
}
