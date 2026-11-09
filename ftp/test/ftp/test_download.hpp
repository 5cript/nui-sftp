#pragma once

#include "test_data_connection.hpp"

#include <ftp/client/transfer.hpp>

#include <gtest/gtest.h>

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
        const TransferSession session{
            .control = connection,
            .passiveChoice = std::make_shared<PassiveChoice>(),
            .restartStream = true,
        };
        auto const download =
            run(connection,
                [session, options]() -> asio::awaitable<std::shared_ptr<Download>>
                {
                    auto started = co_await Download::start(session, {.path = "file"}, options);
                    co_return started ? *started : nullptr;
                });
        ASSERT_TRUE(download);
        const auto readUpTo = [&](std::size_t size)
        {
            return run(
                connection,
                [download, size]() -> asio::awaitable<DownloadResult>
                {
                    DownloadOutcome outcome{};
                    std::array<char, 16> buffer{};
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
            );
        };
        const auto first = readUpTo(3u);
        ASSERT_TRUE(first) << first.error().toString();
        EXPECT_EQ(first->content, "abc");
        // Nobody reads now, the download drops its data connection and the control connection drains.
        ASSERT_TRUE(server_->waitForCheckpoint("drained"));
        const auto rest = readUpTo(100u);
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_EQ(rest->content, "rest");
        const auto finished =
            run(connection,
                [download]() -> asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>>
                {
                    co_return co_await download->finish();
                });
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
        auto script = securedPrefix();
        retrieveWith(script, DataPlan{}.tls({.refuseResumption = true}), "150 Here it comes")
            .beginData()
            .awaitData()
            .reply("522 SSL connection failed: session reuse required")
            .expectDisconnect();
        const auto tls = settings();
        const auto connection = securedWith(std::move(script), tls);
        expectDownloadError(download(connection, {.path = "file"}, tls), RemoteIo::RemoteErrorKind::NoSecureConnection);
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
