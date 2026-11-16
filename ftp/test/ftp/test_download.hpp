#pragma once

#include "test_data_connection.hpp"

#include <ftp/client/transfer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        struct DownloadOutcome
        {
            std::string content{};
            TransferResult result{};
            std::optional<std::uint64_t> announced{};
        };
        using DownloadResult = std::expected<DownloadOutcome, RemoteIo::RemoteError>;

        TransferOptions quickTransferOptions()
        {
            return TransferOptions{.data = quickDataOptions(), .finalReplyTimeout = 2s};
        }

        asio::awaitable<DownloadResult>
        downloadAll(TransferSession session, DownloadRequest request, TransferOptions options)
        {
            auto started = co_await Download::start(std::move(session), std::move(request), options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            auto const download = std::move(started).value();
            DownloadOutcome outcome{.announced = download->announcedSize()};
            std::array<char, 4096> buffer{};
            std::expected<std::size_t, RemoteIo::RemoteError> read = std::size_t{1u};
            while (read && *read > 0u)
            {
                read = co_await download->readSome(asio::buffer(buffer));
                if (read)
                    outcome.content.append(buffer.data(), *read);
            }
            if (!read)
                co_return std::unexpected(read.error());
            auto finished = co_await download->finish();
            if (!finished)
                co_return std::unexpected(std::move(finished).error());
            outcome.result = std::move(*finished);
            co_return outcome;
        }

        /**
         * @brief Reads until @p size bytes came or the data ended.
         */
        asio::awaitable<DownloadResult> readPart(std::shared_ptr<Download> download, std::size_t size)
        {
            DownloadOutcome outcome{};
            std::array<char, 16u * 1024u> buffer{};
            std::expected<std::size_t, RemoteIo::RemoteError> read = std::size_t{1u};
            while (read && *read > 0u && outcome.content.size() < size)
            {
                read = co_await download->readSome(asio::buffer(buffer));
                if (read)
                    outcome.content.append(buffer.data(), *read);
            }
            if (!read)
                co_return std::unexpected(read.error());
            co_return outcome;
        }

        /**
         * @brief EPSV with @p plan behind it, then RETR @p path answered by @p preliminary.
         */
        Script& retrieveWith(Script& script, DataPlan plan, std::string_view preliminary, std::string path = "file")
        {
            return script.expect("EPSV").passive(std::move(plan)).expect("RETR", std::move(path)).reply(preliminary);
        }
    }

    class DownloadTests : public DataConnectionTlsTests
    {
      protected:
        DownloadResult download(
            std::shared_ptr<ControlConnection> const& connection,
            DownloadRequest request,
            std::optional<TlsConnectionSettings> tls = std::nullopt,
            TransferOptions options = quickTransferOptions()
        )
        {
            TransferSession session{
                .control = connection,
                .passiveChoice = std::make_shared<PassiveChoice>(),
                .tls = std::move(tls),
            };
            return run(
                connection,
                [session = std::move(session),
                    request = std::move(request),
                    options]() -> asio::awaitable<DownloadResult>
                {
                    co_return co_await downloadAll(session, request, options);
                }
            );
        }

        std::shared_ptr<ControlConnection> plainConnection(Script script)
        {
            server_->addConnection(std::move(script));
            const auto connection = makeConnection();
            connect(connection);
            return connection;
        }

        void expectDownloadError(DownloadResult const& result, RemoteIo::RemoteErrorKind kind)
        {
            ASSERT_FALSE(result) << "downloaded '" << result->content << "'";
            EXPECT_EQ(result.error().kind, kind) << result.error().toString();
        }

        /**
         * @brief A session whose server continues transfers with REST, so a paused download may drop its data.
         */
        static TransferSession restartingSession(std::shared_ptr<ControlConnection> const& connection)
        {
            return TransferSession{
                .control = connection,
                .passiveChoice = std::make_shared<PassiveChoice>(),
                .restartStream = true,
            };
        }

        /**
         * @brief The download of "file" once it started, nothing read yet.
         */
        std::shared_ptr<Download> startDownload(TransferSession const& session, TransferOptions const& options)
        {
            return run(
                session.control,
                [session, options]() -> asio::awaitable<std::shared_ptr<Download>>
                {
                    auto started = co_await Download::start(session, {.path = "file"}, options);
                    co_return started ? *started : nullptr;
                }
            );
        }

        /**
         * @brief Reads until @p size bytes came or the data ended.
         */
        DownloadResult readUpTo(
            std::shared_ptr<ControlConnection> const& connection,
            std::shared_ptr<Download> const& download,
            std::size_t size
        )
        {
            return run(
                connection,
                [download, size]() -> asio::awaitable<DownloadResult>
                {
                    co_return co_await readPart(download, size);
                }
            );
        }

        /**
         * @brief How the server saw the client end the first data connection, once the client is gone. Nothing when
         *        the server ended that connection itself.
         */
        std::optional<std::string> dataEndAfterDisconnect(std::shared_ptr<ControlConnection> const& connection)
        {
            connection->close();
            EXPECT_TRUE(server_->waitUntilDisconnected());
            const auto transcript = server_->transcript();
            const auto ended = std::ranges::find_if(
                transcript,
                [](TranscriptEntry const& entry)
                {
                    return entry.kind == TranscriptEntry::Kind::DataEnded;
                }
            );
            if (ended == transcript.end())
                return std::nullopt;
            return ended->detail;
        }

        std::expected<void, RemoteIo::RemoteError> seekTo(
            std::shared_ptr<ControlConnection> const& connection,
            std::shared_ptr<Download> const& download,
            std::uint64_t position
        )
        {
            return run(
                connection,
                [download, position]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    co_return download->seek(position);
                }
            );
        }

        std::expected<TransferResult, RemoteIo::RemoteError>
        finishDownload(std::shared_ptr<ControlConnection> const& connection, std::shared_ptr<Download> const& download)
        {
            return run(
                connection,
                [download]() -> asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>>
                {
                    co_return co_await download->finish();
                }
            );
        }
    };

    TEST_F(DownloadTests, DownloadsTheWholeFile)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.send("file content", 5u), "150 Opening BINARY mode data connection for file (12 bytes)."
        )
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = download(connection, {.path = "file"});
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->content, "file content");
        EXPECT_EQ(result->result.bytes, 12u);
        ASSERT_TRUE(result->result.finalReply);
        EXPECT_EQ(result->result.finalReply->code, 226);
        EXPECT_EQ(result->announced, std::optional<std::uint64_t>{12u});
    }

    TEST_F(DownloadTests, OffsetSendsRestJustBeforeRetrieve)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{}.send("content"))
            .expect("REST", "5")
            .reply("350 Restarting at 5.")
            .expect("RETR", "file")
            .reply("150 Opening BINARY mode data connection for file (12 bytes).")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = download(connection, {.path = "file", .offset = 5u, .expectedSize = 12u});
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->content, "content");
        EXPECT_EQ(result->result.bytes, 7u);
        // A size announced behind REST may name the whole file or the rest, it is not used.
        EXPECT_FALSE(result->announced);
    }

    TEST_F(DownloadTests, RefusedRestartFails)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{})
            .expect("REST", "5")
            .reply("502 REST not implemented.")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        expectDownloadError(
            download(connection, {.path = "file", .offset = 5u}), RemoteIo::RemoteErrorKind::Unsupported
        );
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(DownloadTests, RefusedRetrieveLeavesTheControlConnectionUsable)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(script, DataPlan{}, "550 Failed to open file: No such file or directory")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        expectDownloadError(download(connection, {.path = "file"}), RemoteIo::RemoteErrorKind::NoSuchFile);
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(DownloadTests, FinalReplyBeforeAnyPreliminaryOne)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(script, DataPlan{}.send("small"), "226 Transfer complete.")
            .beginData()
            .awaitData()
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = download(connection, {.path = "file"});
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->content, "small");
        EXPECT_EQ(result->result.finalReply->code, 226);
    }

    TEST_F(DownloadTests, DataThatEndsEarlyIsTruncated)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.send("0123456789"), "150 Opening BINARY mode data connection for file (100 bytes)."
        )
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expect("SIZE", "file")
            .reply("213 100")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        expectDownloadError(download(connection, {.path = "file"}), RemoteIo::RemoteErrorKind::Truncated);
    }

    TEST_F(DownloadTests, FileThatChangedMeanwhileIsComplete)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.send("0123456789"), "150 Opening BINARY mode data connection for file (100 bytes)."
        )
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expect("SIZE", "file")
            .reply("213 10")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = download(connection, {.path = "file"});
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->result.bytes, 10u);
    }

    TEST_F(DownloadTests, MissingFinalReplyWithTheWholeSizeIsComplete)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(script, DataPlan{}.send("12345"), "150 Opening BINARY mode data connection for file (5 bytes).")
            .beginData()
            .awaitData()
            .stall();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.finalReplyTimeout = 300ms;
        const auto result = download(connection, {.path = "file"}, std::nullopt, options);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->content, "12345");
        EXPECT_FALSE(result->result.finalReply);
        // Its replies are out of step now.
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(DownloadTests, MissingFinalReplyWithoutAKnownSizeTimesOut)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(script, DataPlan{}.send("12345"), "150 Here it comes.").beginData().awaitData().stall();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.finalReplyTimeout = 300ms;
        expectDownloadError(
            download(connection, {.path = "file"}, std::nullopt, options), RemoteIo::RemoteErrorKind::TimedOut
        );
    }

    TEST_F(DownloadTests, NegativeFinalReplyFails)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(script, DataPlan{}.send("part"), "150 Here it comes.")
            .beginData()
            .awaitData()
            .reply("451 Local error in processing.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const auto result = download(connection, {.path = "file"});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().protocolCode, 451);
    }

    TEST_F(DownloadTests, SilentDataAbortsTheTransfer)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(script, DataPlan{}.stallAfter("ab"), "150 Here it comes.")
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.data.inactivityTimeout = 300ms;
        expectDownloadError(
            download(connection, {.path = "file"}, std::nullopt, options), RemoteIo::RemoteErrorKind::TimedOut
        );
        // The connection drains to the marker and stays usable.
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"EPSV", "RETR file", "ABOR", "PWD"}));
    }

    TEST_F(DownloadTests, AbortByItsOwnerDrainsAndKeepsTheLogin)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.stallAfter("abc"), "150 Opening BINARY mode data connection for file (7 bytes)."
        )
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto const download = startDownload(
            {.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()}, quickTransferOptions()
        );
        ASSERT_TRUE(download);
        const auto first = readUpTo(connection, download, 3u);
        ASSERT_TRUE(first) << first.error().toString();
        run(connection,
            [download]() -> asio::awaitable<void>
            {
                download->abort();
                co_return;
            });
        const auto after = readUpTo(connection, download, 100u);
        ASSERT_FALSE(after);
        EXPECT_EQ(after.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << after.error().toString();
        const auto finished = finishDownload(connection, download);
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << finished.error().toString();
        const auto sought = seekTo(connection, download, 0u);
        ASSERT_FALSE(sought);
        EXPECT_EQ(sought.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << sought.error().toString();
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(DownloadTests, CancelledReadAbortsTheDownloadAndKeepsTheLogin)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.stallAfter("abc"), "150 Opening BINARY mode data connection for file (7 bytes)."
        )
            .beginData()
            .abortSequence(AbortReplies::ProftpdPlainLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto const download = startDownload(
            {.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()}, quickTransferOptions()
        );
        ASSERT_TRUE(download);
        const auto first = readUpTo(connection, download, 3u);
        ASSERT_TRUE(first) << first.error().toString();
        // Nothing more comes, the read waits for the cancel.
        const auto cancelled =
            run(connection,
                [connection, download]() -> asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
                {
                    std::array<char, 16> buffer{};
                    const auto [exception, result] = co_await asio::co_spawn(
                        connection->strand(),
                        download->readSome(asio::buffer(buffer)),
                        asio::cancel_after(200ms, asio::as_tuple(asio::use_awaitable))
                    );
                    if (exception)
                        std::rethrow_exception(exception);
                    co_return result;
                });
        ASSERT_FALSE(cancelled);
        EXPECT_EQ(cancelled.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << cancelled.error().toString();
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(DownloadTests, SeekToTheReachedPositionCostsNothing)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(script, DataPlan{}.send("abcdefg"), "150 Opening BINARY mode data connection for file (7 bytes).")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto const download = startDownload(
            {.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()}, quickTransferOptions()
        );
        ASSERT_TRUE(download);
        ASSERT_TRUE(seekTo(connection, download, 0u));
        const auto first = readUpTo(connection, download, 3u);
        ASSERT_TRUE(first) << first.error().toString();
        ASSERT_TRUE(seekTo(connection, download, first->content.size()));
        const auto rest = readUpTo(connection, download, 100u);
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(first->content + rest->content, "abcdefg");
        const auto finished = finishDownload(connection, download);
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, 7u);
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"EPSV", "RETR file"}));
    }

    TEST_F(DownloadTests, SeekBackStartsAgainWithRestOnTheSameLogin)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.stallAfter("abcde"), "150 Opening BINARY mode data connection for file (7 bytes)."
        )
            .beginData()
            .abortSequence(AbortReplies::ProftpdPlainLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expect("EPSV")
            .passive(DataPlan{}.send("cdefg"))
            .expect("REST", "2")
            .reply("350 Restarting at 2.")
            .expect("RETR", "file")
            .reply("150 Opening BINARY mode data connection for file (5 bytes).")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto const download = startDownload(
            {.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()}, quickTransferOptions()
        );
        ASSERT_TRUE(download);
        const auto first = readUpTo(connection, download, 5u);
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(first->content, "abcde");
        ASSERT_TRUE(seekTo(connection, download, 2u));
        const auto rest = readUpTo(connection, download, 100u);
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(rest->content, "cdefg");
        // The 7 bytes the first reply announced are the whole file, 5 of them lie behind the new position.
        const auto finished = finishDownload(connection, download);
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, 5u);
    }

    TEST_F(DownloadTests, SeekToTheStartSendsNoRest)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.stallAfter("abc"), "150 Opening BINARY mode data connection for file (7 bytes)."
        )
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory);
        retrieveWith(script, DataPlan{}.send("abcdefg"), "150 Opening BINARY mode data connection for file (7 bytes).")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto const download = startDownload(
            {.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()}, quickTransferOptions()
        );
        ASSERT_TRUE(download);
        const auto first = readUpTo(connection, download, 3u);
        ASSERT_TRUE(first) << first.error().toString();
        ASSERT_TRUE(seekTo(connection, download, 0u));
        const auto whole = readUpTo(connection, download, 100u);
        ASSERT_TRUE(whole) << whole.error().toString();
        EXPECT_EQ(whole->content, "abcdefg");
        const auto finished = finishDownload(connection, download);
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, 7u);
    }

    TEST_F(DownloadTests, PausedDownloadDropsItsDataAndContinuesWithRest)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.stallAfter("abc"), "150 Opening BINARY mode data connection for file (7 bytes)."
        )
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expect("EPSV")
            .passive(DataPlan{}.send("rest"))
            .expect("REST", "3")
            .reply("350 Restarting at 3.")
            .expect("RETR", "file")
            .reply("150 Opening BINARY mode data connection for file (4 bytes).")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.pauseDropAfter = 200ms;
        auto const download = startDownload(restartingSession(connection), options);
        ASSERT_TRUE(download);
        const auto first = readUpTo(connection, download, 3u);
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(first->content, "abc");
        // Nobody reads now, the download drops its data connection and the control connection drains.
        ASSERT_TRUE(server_->waitForCheckpoint("drained"));
        const auto rest = readUpTo(connection, download, 100u);
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(rest->content, "rest");
        const auto finished = finishDownload(connection, download);
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, 7u);
    }

    TEST_F(DownloadTests, DownloadNobodyReadsDropsItsDataAndStartsOver)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.stallAfter("abc"), "150 Opening BINARY mode data connection for file (7 bytes)."
        )
            .beginData()
            .abortSequence(AbortReplies::VsftpdLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained");
        // Nothing was read, so no REST goes out.
        retrieveWith(script, DataPlan{}.send("abcdefg"), "150 Opening BINARY mode data connection for file (7 bytes).")
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.pauseDropAfter = 200ms;
        auto const download = startDownload(restartingSession(connection), options);
        ASSERT_TRUE(download);
        ASSERT_TRUE(server_->waitForCheckpoint("drained"));
        const auto whole = readUpTo(connection, download, 100u);
        ASSERT_TRUE(whole) << whole.error().toString();
        EXPECT_EQ(whole->content, "abcdefg");
        const auto finished = finishDownload(connection, download);
        ASSERT_TRUE(finished) << finished.error().toString();
        EXPECT_EQ(finished->bytes, 7u);
    }

    TEST_F(DownloadTests, SlowDataIsNoPause)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.send("abcdef", 1u, 100ms), "150 Opening BINARY mode data connection for file (6 bytes)."
        )
            .beginData()
            .awaitData()
            .reply("226 Transfer complete.")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        // The timer ends during every read, a read under way is no pause.
        options.pauseDropAfter = 30ms;
        const TransferSession session{
            .control = connection,
            .passiveChoice = std::make_shared<PassiveChoice>(),
            .restartStream = true,
        };
        const auto result =
            run(connection,
                [session, options]() -> asio::awaitable<DownloadResult>
                {
                    co_return co_await downloadAll(session, {.path = "file"}, options);
                });
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->content, "abcdef");
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"EPSV", "RETR file"}));
    }

    TEST_F(DownloadTests, PauseWithoutRestartSupportKeepsTheDataConnection)
    {
        Script script{};
        script.reply("220 Ready");
        retrieveWith(
            script, DataPlan{}.stallAfter("abc"), "150 Opening BINARY mode data connection for file (7 bytes)."
        )
            .beginData()
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        auto options = quickTransferOptions();
        options.pauseDropAfter = 100ms;
        const TransferSession session{.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()};
        auto download =
            run(connection,
                [session, options]() -> asio::awaitable<std::shared_ptr<Download>>
                {
                    auto started = co_await Download::start(session, {.path = "file"}, options);
                    co_return started ? *started : nullptr;
                });
        ASSERT_TRUE(download);
        const auto first =
            run(connection,
                [download]() -> asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
                {
                    std::array<char, 16> buffer{};
                    co_return co_await download->readSome(asio::buffer(buffer));
                });
        ASSERT_TRUE(first) << first.error().toString();
        // Only the clock tells a drop from none: well past pauseDropAfter, nothing may have been aborted.
        std::this_thread::sleep_for(400ms);
        download.reset();
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"EPSV", "RETR file"}));
    }

    TEST_F(DownloadTests, CancelledRestartIsClearedWithRestZero)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("EPSV")
            .passive(DataPlan{})
            .expect("REST", "5")
            .checkpoint("restart")
            .awaitRelease("answer")
            .reply("350 Restarting at 5.")
            .expect("REST", "0")
            .reply("350 Restarting at 0.")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto connection = plainConnection(std::move(script));
        const TransferSession session{.control = connection, .passiveChoice = std::make_shared<PassiveChoice>()};
        const auto signal = std::make_shared<asio::cancellation_signal>();
        auto starting = asio::co_spawn(
            connection->strand(),
            Download::start(session, {.path = "file", .offset = 5u}, quickTransferOptions()),
            asio::bind_cancellation_slot(signal->slot(), asio::use_future)
        );
        ASSERT_TRUE(server_->waitForCheckpoint("restart"));
        asio::post(
            connection->strand(),
            [signal]()
            {
                signal->emit(asio::cancellation_type::terminal);
            }
        );
        ASSERT_EQ(starting.wait_for(20s), std::future_status::ready);
        const auto started = starting.get();
        ASSERT_FALSE(started);
        EXPECT_EQ(started.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << started.error().toString();
        // The 350 comes after the cancel, the server holds the restart until REST 0.
        server_->release("answer");
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    class DownloadTlsTests : public DownloadTests
    {
      protected:
        /**
         * @brief A TLS control connection whose tickets were read, @p rest scripted behind its handshake and NOOP.
         */
        std::shared_ptr<ControlConnection> securedWith(Script script, TlsConnectionSettings const& tls)
        {
            server_->addConnection(std::move(script));
            return securedConnection(tls);
        }

        Script securedPrefix()
        {
            auto script = explicitTls();
            script.expect("NOOP").reply("200 Fine");
            return script;
        }

        struct RefusedDownload
        {
            DownloadResult result{};
            ControlState state{};
        };

        /**
         * @brief A download whose data connection made a full handshake and resumed nothing, and whose server answers
         *        @p refusal behind the 150.
         */
        RefusedDownload downloadRefusedWith(std::string_view refusal)
        {
            auto script = securedPrefix();
            retrieveWith(script, DataPlan{}.tls({.refuseResumption = true}), "150 Here it comes")
                .beginData()
                .awaitData()
                .reply(refusal)
                .expectDisconnect();
            const auto tls = settings();
            const auto connection = securedWith(std::move(script), tls);
            auto result = download(connection, {.path = "file"}, tls);
            return RefusedDownload{.result = std::move(result), .state = stateOf(connection)};
        }

        void expectSessionReuseRequired(RefusedDownload const& refused)
        {
            expectDownloadError(refused.result, RemoteIo::RemoteErrorKind::NoSecureConnection);
            EXPECT_EQ(refused.state, ControlState::Closed);
        }
    };

    TEST_F(DownloadTlsTests, DownloadResumesTheControlSession)
    {
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls({.requireResumption = true}).send("secret"), "150 Opening (6 bytes)")
            .beginData()
            .awaitData()
            .reply("226 Done")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const auto result = download(connection, {.path = "file"}, tls);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->content, "secret");
        EXPECT_TRUE(result->result.resumedSession);
    }

    TEST_F(DownloadTlsTests, TlsDownloadCancelledBehindItsHandshakeClosesWithTheServersAnswer)
    {
        // The 150 stays out, so the start waits with its data handshake done until the cancel. The ABOR goes out
        // with the 150, and the server ends the data connection for it.
        auto script = securedPrefix();
        script.expect("EPSV")
            .passive(DataPlan{}.tls().stallAfter(""))
            .expect("RETR", "file")
            .beginData()
            .awaitRelease("cancelled")
            .reply("150 Here it comes")
            .abortSequence(AbortReplies::ProftpdTlsLike)
            .stopData()
            .expect("PWD")
            .reply(currentDirectory)
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const TransferSession session{
            .control = connection,
            .passiveChoice = std::make_shared<PassiveChoice>(),
            .tls = tls,
        };
        const auto started =
            run(connection,
                [connection, session]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    auto [exception, result] = co_await asio::co_spawn(
                        connection->strand(),
                        Download::start(session, {.path = "file"}, quickTransferOptions()),
                        asio::cancel_after(300ms, asio::as_tuple(asio::use_awaitable))
                    );
                    if (exception)
                        std::rethrow_exception(exception);
                    co_return result.transform([](std::shared_ptr<Download> const&) {});
                });
        ASSERT_FALSE(started);
        EXPECT_EQ(started.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << started.error().toString();
        server_->release("cancelled");
        expectReply(execute(connection, command(Verb::Noop)), 200);
        EXPECT_EQ(dataEndAfterDisconnect(connection), std::nullopt);
    }

    TEST_F(DownloadTlsTests, TlsEndWithoutCloseNotifyAndTheWholeSizeIsComplete)
    {
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls({.closeWithoutCloseNotify = true}).send("cut"), "150 Opening (3 bytes)")
            .beginData()
            .awaitData()
            .reply("226 Done")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const auto result = download(connection, {.path = "file"}, tls);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->content, "cut");
    }

    TEST_F(DownloadTlsTests, TlsEndWithoutCloseNotifyAsksForTheSize)
    {
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls({.closeWithoutCloseNotify = true}).send("cut"), "150 Here it comes")
            .beginData()
            .awaitData()
            .reply("226 Done")
            .expect("SIZE", "file")
            .reply("213 10")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        expectDownloadError(download(connection, {.path = "file"}, tls), RemoteIo::RemoteErrorKind::Truncated);
    }

    TEST_F(DownloadTlsTests, RefusalForAMissingSessionReuseIsNoSecureConnection)
    {
        // What vsftpd sends.
        expectSessionReuseRequired(downloadRefusedWith("522 SSL connection failed: session reuse required"));
    }

    TEST_F(DownloadTlsTests, RefusalOfProftpdForAMissingSessionReuseIsNoSecureConnection)
    {
        expectSessionReuseRequired(downloadRefusedWith("425 Unable to build data connection: Operation not permitted"));
    }

    TEST_F(DownloadTlsTests, RefusalThatNamesTheSessionIsNoSecureConnection)
    {
        // What FileZilla Server sends.
        expectSessionReuseRequired(downloadRefusedWith(
            "450 TLS session of data connection has not resumed or the session does not match the control connection"
        ));
    }

    TEST_F(DownloadTlsTests, OtherRefusalBehindAFullDataHandshakeKeepsTheLogin)
    {
        // No session was resumed, but the server does not ask for that: its refusal has another reason.
        const auto refused = downloadRefusedWith("450 Requested file action not taken.");
        ASSERT_FALSE(refused.result);
        EXPECT_EQ(refused.result.error().protocolCode, 450) << refused.result.error().toString();
        EXPECT_NE(refused.result.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection)
            << refused.result.error().toString();
        EXPECT_EQ(refused.state, ControlState::Open);
    }

    TEST_F(DownloadTlsTests, ContinuedDownloadRefusedForSessionReuseIsNoSecureConnection)
    {
        // The data connection behind the pause resumes nothing. Bytes of the one before it prove no session reuse.
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls({.requireResumption = true}).stallAfter("abc"), "150 Opening (7 bytes)")
            .beginData()
            .abortSequence(AbortReplies::ProftpdTlsLike)
            .expect("PWD")
            .reply(currentDirectory)
            .checkpoint("drained")
            .expect("EPSV")
            .passive(DataPlan{}.tls({.refuseResumption = true}))
            .expect("REST", "3")
            .reply("350 Restarting at 3.")
            .expect("RETR", "file")
            .reply("150 Opening (4 bytes)")
            .beginData()
            .awaitData()
            .reply("522 SSL connection failed: session reuse required")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        auto options = quickTransferOptions();
        options.pauseDropAfter = 200ms;
        auto session = restartingSession(connection);
        session.tls = tls;
        auto const download = startDownload(session, options);
        ASSERT_TRUE(download);
        const auto first = readUpTo(connection, download, 3u);
        ASSERT_TRUE(first) << first.error().toString();
        ASSERT_TRUE(server_->waitForCheckpoint("drained"));
        const auto rest = readUpTo(connection, download, 100u);
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(rest->content, "");
        const auto finished = finishDownload(connection, download);
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << finished.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Closed);
    }

    TEST_F(DownloadTlsTests, RefusalBeforeTheDataHandshakeKeepsTheLogin)
    {
        // The refusal cuts the data handshake short, which proves nothing about session reuse.
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls(), "425 Failed to establish connection.")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const auto result = download(connection, {.path = "file"}, tls);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().protocolCode, 425) << result.error().toString();
        EXPECT_NE(result.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << result.error().toString();
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(DownloadTlsTests, NegativeFinalReplyAfterDataIsTheServersReason)
    {
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls({.refuseResumption = true}).send("part"), "150 Here it comes")
            .beginData()
            .awaitData()
            .reply("450 Transfer aborted.")
            .expect("NOOP")
            .reply("200 Fine")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        const auto result = download(connection, {.path = "file"}, tls);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().protocolCode, 450) << result.error().toString();
        EXPECT_NE(result.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << result.error().toString();
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(DownloadTlsTests, FailedDataHandshakeAbortsTheTransfer)
    {
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls({.insteadOfHandshake = fatalAlert()}), "150 Here it comes")
            .beginData()
            .abortSequence(AbortReplies::ProftpdTlsLike)
            .expect("PWD")
            .reply(currentDirectory)
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        expectDownloadError(download(connection, {.path = "file"}, tls), RemoteIo::RemoteErrorKind::NoSecureConnection);
    }
}
