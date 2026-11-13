#pragma once

#include "test_download.hpp"
#include "test_ftp_file_system.hpp"
#include "test_upload.hpp"

#include <ftp/remote/ftp_file_stream.hpp>
#include <ftp/remote/ftp_file_system.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using StreamPointer = std::shared_ptr<RemoteIo::IRemoteFileStream>;

        constexpr auto readMode = RemoteIo::OpenMode::Read;
        constexpr auto writeMode = RemoteIo::OpenMode::Write | RemoteIo::OpenMode::Create | RemoteIo::OpenMode::Truncate;

        /**
         * @brief SIZE @p path answered with @p size, as a read-open asks.
         */
        Script& sized(Script& script, std::string path, std::string size)
        {
            return script.expect("SIZE", std::move(path)).reply(fmt::format("213 {}", size));
        }

        /**
         * @brief EPSV with @p plan behind it, then @p verb @p path answered with a 150.
         */
        Script& transferring(Script& script, DataPlan plan, std::string verb, std::string path)
        {
            return script.expect("EPSV")
                .passive(std::move(plan))
                .expect(std::move(verb), std::move(path))
                .reply("150 Opening BINARY mode data connection.");
        }

        /**
         * @brief A login as loggedIn() gives, of a server whose FEAT names no REST STREAM.
         */
        Script loggedInWithoutRestart()
        {
            Script script{};
            script.login()
                .expect("FEAT")
                .reply("211-Features:\n EPSV\n MDTM\n SIZE\n211 End")
                .expect("OPTS", "UTF8 ON")
                .reply("200 Always in UTF8 mode.")
                .expect("TYPE", "I")
                .reply("200 Switching to Binary mode.")
                .expect("PWD")
                .reply("257 \"/home/user\" is the current directory");
            return script;
        }

        /**
         * @brief The data runs to its end, then 226.
         */
        Script& completed(Script& script)
        {
            return script.beginData().awaitData().reply("226 Transfer complete.");
        }
    }

    class FtpFileStreamTests : public FtpFileSystemTests
    {
      protected:
        /**
         * @brief Logs in C0 with @p metadata and lets the first transfer connection follow @p transfer.
         */
        std::shared_ptr<FtpFileSystem> connectWith(Script metadata, Script transfer)
        {
            const auto fileSystem = connect(std::move(metadata));
            server_->addConnection(std::move(transfer));
            return fileSystem;
        }

        StreamPointer open(std::string_view path, RemoteIo::OpenMode mode)
        {
            const auto opened = later(RemoteIo::OpenFile{.path = remote(path), .mode = mode});
            if (!opened)
                throw std::runtime_error{fmt::format("The open failed: {}", opened.error().toString())};
            auto stream = opened->lock();
            if (!stream)
                throw std::runtime_error{"The stream was gone after the open"};
            return stream;
        }

        /**
         * @brief Opens @p path, after the wake of a Busy open: a closed stream gives its connection back a few hops
         *        later.
         */
        StreamPointer openWhenFree(std::string_view path, RemoteIo::OpenMode mode)
        {
            constexpr int attempts = 10;
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                const auto woken = std::make_shared<std::promise<void>>();
                auto wake = woken->get_future();
                const auto opened = later(
                    RemoteIo::OpenFile{
                        .path = remote(path),
                        .mode = mode,
                        .wakeWhenFree =
                            [woken]()
                        {
                            woken->set_value();
                        },
                    }
                );
                if (opened)
                {
                    if (auto stream = opened->lock())
                        return stream;
                    throw std::runtime_error{"The stream was gone after the open"};
                }
                if (opened.error().kind != RemoteIo::RemoteErrorKind::Busy)
                    throw std::runtime_error{fmt::format("The open failed: {}", opened.error().toString())};
                if (wake.wait_for(10s) != std::future_status::ready)
                    throw std::runtime_error{"No connection came free"};
            }
            throw std::runtime_error{"The open stayed busy"};
        }

        /**
         * @brief Runs @p function on the processing thread and returns what it returned.
         */
        template <typename FunctionT>
        auto onThread(FunctionT function)
        {
            auto result = thread_.pushPromiseTask(std::move(function));
            return valueOf(result);
        }

        /**
         * @brief Reads to the end in pieces of @p pieceSize, then finishes.
         */
        std::expected<std::string, RemoteIo::RemoteError> readWhole(StreamPointer const& stream, std::size_t pieceSize = 4096u)
        {
            return onThread(
                [stream, pieceSize]() -> std::expected<std::string, RemoteIo::RemoteError>
                {
                    std::string content{};
                    std::vector<char> buffer(pieceSize);
                    auto read = stream->readSomeInStrand(buffer.data(), buffer.size());
                    while (read && *read > 0u)
                    {
                        content.append(buffer.data(), *read);
                        read = stream->readSomeInStrand(buffer.data(), buffer.size());
                    }
                    if (!read)
                        return std::unexpected(read.error());
                    return stream->finishInStrand().transform(
                        [&content]()
                        {
                            return content;
                        }
                    );
                }
            );
        }

        void closeOnThread(StreamPointer const& stream)
        {
            onThread(
                [stream]()
                {
                    stream->closeInStrand();
                }
            );
        }
    };

    TEST_P(FtpFileStreamTests, ADownloadReadsTheFileAndFinishes)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "12");
        completed(transferring(transfer, DataPlan{}.send("file content", 5u), "RETR", "/dir/file")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto content = readWhole(stream, 4u);
        ASSERT_TRUE(content) << content.error().toString();
        EXPECT_EQ(*content, "file content");
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, ASeekBeforeTheFirstReadStartsThereAndTheReachedPositionCostsNothing)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "10");
        transfer.expect("EPSV").passive(DataPlan{}.send("456789")).expect("REST", "4").reply("350 Restart position accepted (4).");
        completed(transfer.expect("RETR", "/dir/file").reply("150 Opening BINARY mode data connection.")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto tail = onThread(
            [stream]() -> std::expected<std::string, RemoteIo::RemoteError>
            {
                if (auto sought = stream->seekInStrand(4u); !sought)
                    return std::unexpected(sought.error());
                std::array<char, 2u> buffer{};
                std::size_t filled = 0u;
                while (filled < buffer.size())
                {
                    const auto read = stream->readSomeInStrand(buffer.data() + filled, buffer.size() - filled);
                    if (!read)
                        return std::unexpected(read.error());
                    if (*read == 0u)
                        break;
                    filled += *read;
                }
                if (auto sought = stream->seekInStrand(6u); !sought)
                    return std::unexpected(sought.error());
                return std::string{buffer.data(), filled};
            }
        );
        ASSERT_TRUE(tail) << tail.error().toString();
        EXPECT_EQ(*tail, "45");
        const auto rest = readWhole(stream);
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(*rest, "6789");
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, AnUploadStoresWhatWasWritten)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        completed(transferring(transfer, DataPlan{}.expectUpload(1024u), "STOR", "/dir/new")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/new", writeMode);
        const auto written = onThread(
            [stream]() -> std::expected<void, RemoteIo::RemoteError>
            {
                if (auto first = stream->writeInStrand("abc"); !first)
                    return first;
                if (auto second = stream->writeInStrand("def"); !second)
                    return second;
                return stream->finishInStrand();
            }
        );
        ASSERT_TRUE(written) << written.error().toString();
        closeOnThread(stream);
        EXPECT_EQ(uploadedContent(server_->transcript()), std::optional<std::string>{"abcdef"});
    }

    TEST_P(FtpFileStreamTests, AnUploadThatWroteNothingStoresAnEmptyFile)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        completed(transferring(transfer, DataPlan{}.expectUpload(1024u), "STOR", "/dir/empty")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/empty", writeMode);
        const auto finished = onThread(
            [stream]()
            {
                return stream->finishInStrand();
            }
        );
        ASSERT_TRUE(finished) << finished.error().toString();
        closeOnThread(stream);
        EXPECT_EQ(uploadedContent(server_->transcript()), std::optional<std::string>{""});
    }

    TEST_P(FtpFileStreamTests, AContinuedUploadRestartsWhereItWasSought)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        transfer.expect("EPSV")
            .passive(DataPlan{}.expectUpload(1024u))
            .expect("REST", "3")
            .reply("350 Restart position accepted (3).");
        completed(transfer.expect("STOR", "/dir/part").reply("150 Ok to send data.")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/part", RemoteIo::OpenMode::Write);
        const auto written = onThread(
            [stream]() -> std::expected<void, RemoteIo::RemoteError>
            {
                if (auto sought = stream->seekInStrand(3u); !sought)
                    return sought;
                if (auto rest = stream->writeInStrand("def"); !rest)
                    return rest;
                return stream->finishInStrand();
            }
        );
        ASSERT_TRUE(written) << written.error().toString();
        closeOnThread(stream);
        EXPECT_EQ(uploadedContent(server_->transcript()), std::optional<std::string>{"def"});
    }

    TEST_P(FtpFileStreamTests, AContinuedUploadWithoutRestartAppendsWhereTheFileEnds)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInWithoutRestart();
        sized(transfer, "/dir/part", "3");
        completed(transferring(transfer, DataPlan{}.expectUpload(1024u), "APPE", "/dir/part")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/part", RemoteIo::OpenMode::Write);
        const auto written = onThread(
            [stream]() -> std::expected<void, RemoteIo::RemoteError>
            {
                if (auto sought = stream->seekInStrand(3u); !sought)
                    return sought;
                if (auto rest = stream->writeInStrand("def"); !rest)
                    return rest;
                return stream->finishInStrand();
            }
        );
        ASSERT_TRUE(written) << written.error().toString();
        closeOnThread(stream);
        EXPECT_EQ(uploadedContent(server_->transcript()), std::optional<std::string>{"def"});
    }

    TEST_P(FtpFileStreamTests, AContinuedUploadWithoutRestartIsRefusedWhereTheFileEndsElsewhere)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInWithoutRestart();
        sized(transfer, "/dir/part", "9").expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/part", RemoteIo::OpenMode::Write);
        const auto written = onThread(
            [stream]() -> std::expected<void, RemoteIo::RemoteError>
            {
                if (auto sought = stream->seekInStrand(3u); !sought)
                    return sought;
                return stream->writeInStrand("def");
            }
        );
        ASSERT_FALSE(written) << "appended to a file that ends elsewhere than the seek";
        EXPECT_EQ(written.error().kind, RemoteIo::RemoteErrorKind::Unsupported) << written.error().toString();
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, OpeningAMissingFileFailsAndTheConnectionServesTheNextOpen)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        transfer.expect("SIZE", "/dir/missing").reply("550 Could not get file size.");
        listsIn(transfer, "/dir", std::string{listedFile});
        sized(transfer, "/dir/a", "5").expectDisconnect();
        connect(std::move(metadata), {}, 1u);
        server_->addConnection(std::move(transfer));

        const auto missing = later(RemoteIo::OpenFile{.path = remote("/dir/missing"), .mode = readMode});
        ASSERT_FALSE(missing) << "opened a missing file";
        EXPECT_EQ(missing.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile) << missing.error().toString();
        const auto stream = openWhenFree("/dir/a", readMode);
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, AFileOpensForReadingOnAServerWithoutSize)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        transfer.expect("SIZE", "/dir/file").reply("502 Command not implemented.");
        completed(transferring(transfer, DataPlan{}.send("file content"), "RETR", "/dir/file")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto content = readWhole(stream);
        ASSERT_TRUE(content) << content.error().toString();
        EXPECT_EQ(*content, "file content");
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, AnOpenIsBusyWhileTheConnectionsTransferAndWakesWhenOneIsFree)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "5");
        sized(transfer, "/dir/b", "7").expectDisconnect();
        connect(std::move(metadata), {}, 1u);
        server_->addConnection(std::move(transfer));

        const auto first = open("/dir/a", readMode);
        std::promise<void> woken{};
        auto wake = woken.get_future();
        const auto busy = later(
            RemoteIo::OpenFile{
                .path = remote("/dir/b"),
                .mode = readMode,
                .wakeWhenFree =
                    [woken = std::make_shared<std::promise<void>>(std::move(woken))]()
                {
                    woken->set_value();
                },
            }
        );
        ASSERT_FALSE(busy) << "opened beyond the transfer connections";
        EXPECT_EQ(busy.error().kind, RemoteIo::RemoteErrorKind::Busy) << busy.error().toString();
        EXPECT_EQ(wake.wait_for(100ms), std::future_status::timeout) << "woken while the connection transfers";

        closeOnThread(first);
        ASSERT_EQ(wake.wait_for(10s), std::future_status::ready) << "the close did not wake the waiting open";
        const auto second = open("/dir/b", readMode);
        closeOnThread(second);
    }

    TEST_P(FtpFileStreamTests, ClosingADownloadThatDidNotFinishAbortsItAndTheConnectionServesTheNextOpen)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "7");
        transferring(transfer, DataPlan{}.stallAfter("abc"), "RETR", "/dir/a")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory);
        sized(transfer, "/dir/b", "1").expectDisconnect();
        connect(std::move(metadata), {}, 1u);
        server_->addConnection(std::move(transfer));

        const auto stream = open("/dir/a", readMode);
        const auto read = onThread(
            [stream]()
            {
                std::array<char, 3u> buffer{};
                return stream->readSomeInStrand(buffer.data(), buffer.size());
            }
        );
        ASSERT_TRUE(read) << read.error().toString();
        EXPECT_GT(*read, 0u);
        closeOnThread(stream);
        const auto next = openWhenFree("/dir/b", readMode);
        closeOnThread(next);
    }

    TEST_P(FtpFileStreamTests, TheCloseOfTheFileSystemEndsItsStreams)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "7");
        transferring(transfer, DataPlan{}.stallAfter("abc"), "RETR", "/dir/a")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        const auto fileSystem = connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/a", readMode);
        const auto read = onThread(
            [stream]()
            {
                std::array<char, 3u> buffer{};
                return stream->readSomeInStrand(buffer.data(), buffer.size());
            }
        );
        ASSERT_TRUE(read) << read.error().toString();
        EXPECT_TRUE(fileSystem->close());
        const auto after = onThread(
            [stream]()
            {
                std::array<char, 3u> buffer{};
                return stream->readSomeInStrand(buffer.data(), buffer.size());
            }
        );
        ASSERT_FALSE(after) << "read after the close";
        EXPECT_EQ(after.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << after.error().toString();
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpFileStreamTests, TheCloseOfTheFileSystemEndsItsStreamsOnceTheProcessingThreadStopped)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "7");
        transferring(transfer, DataPlan{}.stallAfter("abc"), "RETR", "/dir/a")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        const auto fileSystem = connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/a", readMode);
        const auto read = onThread(
            [stream]()
            {
                std::array<char, 3u> buffer{};
                return stream->readSomeInStrand(buffer.data(), buffer.size());
            }
        );
        ASSERT_TRUE(read) << read.error().toString();
        thread_.stop();
        // The thread takes no task any more: the close ends the stream itself, which aborts the download.
        EXPECT_TRUE(fileSystem->close());
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpFileStreamTests, ACloseFromACompletionDoesNotWaitForTheProcessingThread)
    {
        auto metadata = loggedInScript();
        sized(metadata, "/dir/b", "5");
        metadata.expect("MDTM", "/dir/b").reply("213 20200102030405").expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "7").expectDisconnect();
        const auto fileSystem = connectWith(std::move(metadata), std::move(transfer));
        const auto stream = open("/dir/a", readMode);

        // The processing thread is held, as it is inside a call of a stream, which waits for the session strand.
        std::promise<void> release{};
        auto held = thread_.pushPromiseTask(
            [released = release.get_future().share()]()
            {
                released.wait();
            }
        );
        const auto closed = std::make_shared<std::promise<bool>>();
        auto closedFuture = closed->get_future();
        fileSystem->later(
            RemoteIo::Stat{.path = remote("/dir/b")},
            [fileSystem, closed](RemoteIo::OperationResult<RemoteIo::Stat> const&)
            {
                closed->set_value(fileSystem->close());
            }
        );
        const auto returned = closedFuture.wait_for(10s);
        release.set_value();
        ASSERT_EQ(returned, std::future_status::ready) << "the close waited for the processing thread";
        EXPECT_TRUE(closedFuture.get());
        valueOf(held);

        const auto after = onThread(
            [stream]()
            {
                std::array<char, 3u> buffer{};
                return stream->readSomeInStrand(buffer.data(), buffer.size());
            }
        );
        ASSERT_FALSE(after) << "read after the close";
        EXPECT_EQ(after.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << after.error().toString();
    }

    TEST_P(FtpFileStreamTests, ReadingAFileForCommandsStopsAtTheAskedSize)
    {
        auto metadata = loggedInScript();
        completed(transferring(metadata, DataPlan{}.send("0123456789"), "RETR", "/dir/file"));
        transferring(metadata, DataPlan{}.stallAfter("0123456789"), "RETR", "/dir/file")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        connect(std::move(metadata));

        const auto whole = later(RemoteIo::ReadFile{.path = remote("/dir/file"), .maximumSize = 100u});
        ASSERT_TRUE(whole) << whole.error().toString();
        EXPECT_EQ(*whole, "0123456789");
        const auto start = later(RemoteIo::ReadFile{.path = remote("/dir/file"), .maximumSize = 4u});
        ASSERT_TRUE(start) << start.error().toString();
        EXPECT_EQ(*start, "0123");
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpFileStreamTests, AFinalReplyThatRefusesADownloadFailsItsFinish)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "12");
        transferring(transfer, DataPlan{}.send("file content"), "RETR", "/dir/file")
            .beginData()
            .awaitData()
            .reply("451 Read error.")
            .expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto content = readWhole(stream);
        ASSERT_FALSE(content) << "a download the server refused at its end finished";
        EXPECT_EQ(content.error().protocolCode, 451) << content.error().toString();
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, AFinalReplyThatRefusesAnUploadFailsItsFinish)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        transferring(transfer, DataPlan{}.expectUpload(1024u), "STOR", "/dir/new")
            .beginData()
            .awaitData()
            .reply("552 Disk full.")
            .expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/new", writeMode);
        const auto written = onThread(
            [stream]() -> std::expected<void, RemoteIo::RemoteError>
            {
                if (auto first = stream->writeInStrand("abc"); !first)
                    return first;
                return stream->finishInStrand();
            }
        );
        ASSERT_FALSE(written) << "an upload the server refused at its end finished";
        EXPECT_EQ(written.error().protocolCode, 552) << written.error().toString();
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, AStatOfAStreamThatTransfersAsksOnTheConnectionForCommands)
    {
        auto metadata = loggedInScript();
        sized(metadata, "/dir/a", "7");
        metadata.expect("MDTM", "/dir/a").reply("213 20200102030405").expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "7");
        transferring(transfer, DataPlan{}.stallAfter("abc"), "RETR", "/dir/a")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/a", readMode);
        const auto information = onThread(
            [stream]() -> std::expected<RemoteIo::FileInformation, RemoteIo::RemoteError>
            {
                std::array<char, 3u> buffer{};
                if (const auto read = stream->readSomeInStrand(buffer.data(), buffer.size()); !read)
                    return std::unexpected(read.error());
                return stream->statInStrand();
            }
        );
        ASSERT_TRUE(information) << information.error().toString();
        EXPECT_TRUE(information->isRegularFile());
        EXPECT_EQ(information->size, 7u);
        closeOnThread(stream);
        EXPECT_TRUE(server_->waitForCheckpoint("drained"));
    }

    TEST_P(FtpFileStreamTests, AStatOfAStreamOnTheOnlyConnectionAsksThereBeforeAndAfterItsTransfer)
    {
        auto only = loggedInScript();
        sized(only, "/dir/file", "12");
        sized(only, "/dir/file", "12").expect("MDTM", "/dir/file").reply("213 20200102030405");
        completed(transferring(only, DataPlan{}.send("file content"), "RETR", "/dir/file"));
        sized(only, "/dir/file", "12").expect("MDTM", "/dir/file").reply("213 20200102030405").expectDisconnect();
        connect(std::move(only), {}, 1u);
        // The server allows one connection: the stream takes the connection for commands.
        server_->addConnection(refusedAtTheGreeting());

        const auto stream = open("/dir/file", readMode);
        const auto stat = [stream]()
        {
            return stream->statInStrand();
        };
        const auto before = onThread(stat);
        ASSERT_TRUE(before) << before.error().toString();
        EXPECT_EQ(before->size, 12u);
        const auto content = readWhole(stream);
        ASSERT_TRUE(content) << content.error().toString();
        const auto after = onThread(stat);
        ASSERT_TRUE(after) << after.error().toString();
        EXPECT_EQ(after->size, 12u);
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, AStreamMovesBytesOnlyTheWayItWasOpened)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/a", "5").expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto exclusive = later(
            RemoteIo::OpenFile{.path = remote("/dir/new"), .mode = writeMode | RemoteIo::OpenMode::Exclusive}
        );
        ASSERT_FALSE(exclusive) << "promised an exclusive create";
        EXPECT_EQ(exclusive.error().kind, RemoteIo::RemoteErrorKind::Unsupported) << exclusive.error().toString();

        const auto stream = open("/dir/a", readMode);
        const auto written = onThread(
            [stream]()
            {
                return stream->writeInStrand("x");
            }
        );
        ASSERT_FALSE(written) << "wrote to a stream opened for reading";
        closeOnThread(stream);
    }

    TEST_P(FtpFileStreamTests, ReadAllHandsOverTheWholeFile)
    {
        auto metadata = loggedInScript();
        metadata.expectDisconnect();
        auto transfer = loggedInScript();
        sized(transfer, "/dir/file", "12");
        completed(transferring(transfer, DataPlan{}.send("file content", 5u), "RETR", "/dir/file")).expectDisconnect();
        connectWith(std::move(metadata), std::move(transfer));

        const auto stream = open("/dir/file", readMode);
        const auto content = std::make_shared<std::string>();
        auto total = stream->readAll(
            [content](std::string_view data)
            {
                content->append(data);
                return true;
            }
        );
        const auto read = valueOf(total);
        ASSERT_TRUE(read) << read.error().toString();
        EXPECT_EQ(*read, 12u);
        EXPECT_EQ(*content, "file content");
        closeOnThread(stream);
    }

    INSTANTIATE_TEST_SUITE_P(
        Executors,
        FtpFileStreamTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );
}
