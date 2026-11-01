#pragma once

#include "common_fixture.hpp"
#include "test_sftp.hpp"

#include <remote_io/remote_file_system.hpp>
#include <ssh/session.hpp>
#include <ssh/sftp_session.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <string_view>

namespace SecureShell::Test
{
    /**
     * @brief The SFTP session through the protocol neutral interface the backend is moving to.
     */
    class SftpFileSystemTests : public SftpTests
    {
      public:
        /**
         * @brief Runs @p operation with inStrand on the processing thread of @p fileSystem.
         */
        template <typename OperationT>
        RemoteIo::OperationResult<OperationT> runInStrand(RemoteIo::IRemoteFileSystem& fileSystem, OperationT operation)
        {
            auto future = fileSystem.performPromise(
                [&fileSystem, operation = std::move(operation)]()
                {
                    return fileSystem.inStrand(operation);
                }
            );
            if (future.wait_for(replyTimeout) != std::future_status::ready)
                return std::unexpected(RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::TimedOut});
            return future.get();
        }

        /**
         * @brief Runs @p operation with later from the test's thread.
         */
        template <typename OperationT>
        RemoteIo::OperationResult<OperationT> runLater(RemoteIo::IRemoteFileSystem& fileSystem, OperationT operation)
        {
            auto future = fileSystem.later(std::move(operation));
            if (future.wait_for(replyTimeout) != std::future_status::ready)
                return std::unexpected(RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::TimedOut});
            return future.get();
        }
    };

    TEST_F(SftpFileSystemTests, OperationsRunFromAnyThreadAndOnTheProcessingThread)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);

        const auto listed = runLater(*sftp, RemoteIo::ListDirectory{.path = remote("/home/test")});
        ASSERT_TRUE(listed.has_value()) << listed.error().toString();
        EXPECT_TRUE(
            std::ranges::any_of(
                *listed,
                [](auto const& entry)
                {
                    return entry.name == "file1.txt";
                }
            )
        );

        const auto information = runInStrand(*sftp, RemoteIo::Lstat{.path = remote("/home/test/file1.txt")});
        ASSERT_TRUE(information.has_value()) << information.error().toString();
        EXPECT_EQ(information->type, SharedData::FileType::Regular);
    }

    TEST_F(SftpFileSystemTests, AnOperationLaterOnAClosedSessionCompletesUnfinished)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_TRUE(sftp->close());

        const auto information = runLater(*sftp, RemoteIo::Lstat{.path = remote("/home/test/file1.txt")});
        ASSERT_FALSE(information.has_value());
        EXPECT_EQ(information.error().wrapperError, RemoteIo::WrapperErrors::TaskPushFailed);
    }

    TEST_F(SftpFileSystemTests, RenameReplacesAsTheCapabilitiesTell)
    {
        for (const bool posixRename : {false, true})
        {
            CREATE_SERVER_AND_JOINER(Sftp);
            if (posixRename)
                ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "posix-rename@openssh.com"}}));
            auto [owner, sftp] = createSftpSession(serverStartResult->port);

            const auto capabilities = sftp->capabilities();
            EXPECT_EQ(
                capabilities.renameSemantics,
                posixRename ? RemoteIo::RenameSemantics::Replaces : RemoteIo::RenameSemantics::Refuses
            );
            EXPECT_TRUE(capabilities.symlinks);
            EXPECT_TRUE(capabilities.setModificationTime);
            EXPECT_TRUE(capabilities.exclusiveCreate);
            EXPECT_TRUE(capabilities.canonicalPath);

            const auto renamed = runInStrand(
                *sftp,
                RemoteIo::Rename{
                    .source = remote("/home/test/file1.txt"), .destination = remote("/home/test/file2.txt")
                }
            );
            EXPECT_EQ(renamed.has_value(), posixRename) << "posix-rename: " << posixRename;
        }
    }

    TEST_F(SftpFileSystemTests, AnExclusiveOpenCreatesAFileOnlyOnce)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        using enum RemoteIo::OpenMode;
        const auto open =
            RemoteIo::OpenFile{.path = remote("/home/test/created.txt"), .mode = Write | Create | Exclusive};

        auto written = sftp->performPromise(
            [&sftp, &open]()
            {
                return sftp->inStrand(open).and_then(
                    [](std::weak_ptr<RemoteIo::IRemoteFileStream> const& weakStream)
                    {
                        const auto stream = weakStream.lock();
                        auto result = stream->writeInStrand("created").and_then(
                            [&stream]()
                            {
                                return stream->finishInStrand();
                            }
                        );
                        stream->closeInStrand();
                        return result;
                    }
                );
            }
        );
        ASSERT_EQ(written.wait_for(replyTimeout), std::future_status::ready);
        const auto writeResult = written.get();
        ASSERT_TRUE(writeResult.has_value()) << writeResult.error().toString();
        EXPECT_EQ(readRemote(*sftp, "/home/test/created.txt"), "created");

        const auto again = runInStrand(*sftp, open);
        EXPECT_FALSE(again.has_value());
        EXPECT_EQ(readRemote(*sftp, "/home/test/created.txt"), "created");
    }

    TEST_F(SftpFileSystemTests, FinishReportsTheAnswerToTheClose)
    {
        for (const bool failClose : {false, true})
        {
            CREATE_SERVER_AND_JOINER(Sftp);
            if (failClose)
                ASSERT_TRUE(serverStartResult->command("failCloses", nlohmann::json::object()));
            auto [owner, sftp] = createSftpSession(serverStartResult->port);

            std::shared_ptr<RemoteIo::IRemoteFileStream> stream{};
            auto finished = sftp->performPromise(
                [&sftp, &stream]()
                {
                    using enum RemoteIo::OpenMode;
                    return sftp
                        ->inStrand(
                            RemoteIo::OpenFile{.path = remote("/home/test/finished.txt"), .mode = Write | Create}
                        )
                        .and_then(
                            [&stream](
                                std::weak_ptr<RemoteIo::IRemoteFileStream> const& weakStream
                            ) -> std::expected<void, RemoteIo::RemoteError>
                            {
                                stream = weakStream.lock();
                                if (!stream)
                                    return std::unexpected(RemoteIo::RemoteError{.message = "The stream is gone"});
                                return stream->writeInStrand("finished");
                            }
                        )
                        .and_then(
                            [&stream]()
                            {
                                return stream->finishInStrand();
                            }
                        );
                }
            );
            ASSERT_EQ(finished.wait_for(replyTimeout), std::future_status::ready);
            const auto finishResult = finished.get();
            EXPECT_EQ(finishResult.has_value(), !failClose) << "failClose: " << failClose;
            ASSERT_NE(stream, nullptr);

            auto writtenAfterFinish = sftp->performPromise(
                [&stream]()
                {
                    auto written = stream->writeInStrand("late");
                    stream->closeInStrand();
                    return written;
                }
            );
            ASSERT_EQ(writtenAfterFinish.wait_for(replyTimeout), std::future_status::ready);
            EXPECT_FALSE(writtenAfterFinish.get().has_value());
            stream.reset();
            EXPECT_EQ(readRemote(*sftp, "/home/test/finished.txt"), "finished");
        }
    }

    TEST_F(SftpFileSystemTests, SetModificationTimeSetsWhatStatReports)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);
        constexpr std::uint64_t seconds = 1'700'000'000u;

        const auto set = runInStrand(
            *sftp,
            RemoteIo::SetModificationTime{
                .path = remote("/home/test/file1.txt"),
                .modificationTime = std::chrono::system_clock::time_point{std::chrono::seconds{seconds}},
            }
        );
        ASSERT_TRUE(set.has_value()) << set.error().toString();

        const auto information = runLater(*sftp, RemoteIo::Stat{.path = remote("/home/test/file1.txt")});
        ASSERT_TRUE(information.has_value()) << information.error().toString();
        EXPECT_EQ(information->mtime, seconds);
    }

    TEST_F(SftpFileSystemTests, TransferLimitsAreTheLimitsOfTheStreams)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        auto [owner, sftp] = createSftpSession(serverStartResult->port);

        const auto limits = runInStrand(*sftp, RemoteIo::ReadTransferLimits{});
        ASSERT_TRUE(limits.has_value()) << limits.error().toString();
        const auto stream = openForReading(*sftp, "/home/test/file1.txt");
        ASSERT_NE(stream, nullptr);
        EXPECT_GT(limits->maxWriteLength, 0u);
        EXPECT_EQ(limits->maxWriteLength, stream->writeLengthLimit());
        stream->close();
    }

    TEST_F(SftpFileSystemTests, LinkIntoPlaceIsUnsupportedWithoutHardlinks)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}})
        );
        auto [owner, sftp] = createSftpSession(serverStartResult->port);

        const auto linked = runInStrand(
            *sftp,
            RemoteIo::LinkIntoPlace{
                .source = remote("/home/test/source.txt"), .destination = remote("/home/test/moved.txt")
            }
        );
        ASSERT_FALSE(linked.has_value());
        EXPECT_EQ(linked.error().kind, RemoteIo::RemoteErrorKind::Unsupported);
        EXPECT_EQ(readRemote(*sftp, "/home/test/source.txt"), "source");
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/moved.txt"));
    }

    TEST_F(SftpFileSystemTests, LinkIntoPlaceMovesToAFreeNameAndRefusesAnExistingOne)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}})
        );
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/target.txt"}, {"content", "target"}})
        );
        ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "hardlink@openssh.com"}}));
        auto [owner, sftp] = createSftpSession(serverStartResult->port);

        const auto refused = runInStrand(
            *sftp,
            RemoteIo::LinkIntoPlace{
                .source = remote("/home/test/source.txt"), .destination = remote("/home/test/target.txt")
            }
        );
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().kind, RemoteIo::RemoteErrorKind::AlreadyExists);
        EXPECT_EQ(readRemote(*sftp, "/home/test/target.txt"), "target");

        const auto linked = runInStrand(
            *sftp,
            RemoteIo::LinkIntoPlace{
                .source = remote("/home/test/source.txt"), .destination = remote("/home/test/moved.txt")
            }
        );
        ASSERT_TRUE(linked.has_value()) << linked.error().toString();
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/source.txt"));
        EXPECT_EQ(readRemote(*sftp, "/home/test/moved.txt"), "source");
    }

    TEST_F(SftpFileSystemTests, ALinkTheServerRefusesToAFreeNameIsUnsupported)
    {
        CREATE_SERVER_AND_JOINER(Sftp);
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/source.txt"}, {"content", "source"}})
        );
        ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "hardlink@openssh.com"}}));
        ASSERT_TRUE(serverStartResult->command("failHardlinks", nlohmann::json::object()));
        auto [owner, sftp] = createSftpSession(serverStartResult->port);

        const auto linked = runInStrand(
            *sftp,
            RemoteIo::LinkIntoPlace{
                .source = remote("/home/test/source.txt"), .destination = remote("/home/test/moved.txt")
            }
        );
        ASSERT_FALSE(linked.has_value());
        EXPECT_EQ(linked.error().kind, RemoteIo::RemoteErrorKind::Unsupported);
        EXPECT_EQ(readRemote(*sftp, "/home/test/source.txt"), "source");
        EXPECT_FALSE(remoteExists(*sftp, "/home/test/moved.txt"));
    }
}
