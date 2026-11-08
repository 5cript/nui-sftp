#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/support/scripted_ftp_server.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        namespace asio = boost::asio;
        using namespace std::chrono_literals;
        using RemoteIo::RemoteErrorKind;
        using ReplyResult = std::expected<Reply, RemoteIo::RemoteError>;

        /** @brief Generous, so only the tests about time see a timeout, also under the sanitizers. */
        ControlConnectionOptions relaxedOptions()
        {
            return ControlConnectionOptions{.connectTimeout = 10s, .commandTimeout = 10s};
        }

        Command command(Verb verb, std::string argument = {})
        {
            return Command{.verb = verb, .argument = std::move(argument)};
        }

        void expectReply(ReplyResult const& reply, int code)
        {
            ASSERT_TRUE(reply) << reply.error().toString();
            EXPECT_EQ(reply->code, code);
        }

        void expectError(ReplyResult const& reply, RemoteErrorKind kind)
        {
            ASSERT_FALSE(reply) << "got a " << reply->code << " reply";
            EXPECT_EQ(reply.error().kind, kind) << reply.error().toString();
        }
    }

    class ControlConnectionTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            auto started = ScriptedFtpServer::start();
            ASSERT_TRUE(started) << started.error();
            server_ = std::move(started).value();
        }

        void TearDown() override
        {
            for (auto const& connection : connections_)
                connection->close();
            if (!server_->waitUntilDisconnected())
            {
                std::string transcript{};
                for (auto const& entry : server_->transcript())
                    transcript +=
                        fmt::format("\n  {} {}: {}", entry.connection, static_cast<int>(entry.kind), entry.detail);
                ADD_FAILURE() << "The server still has a connection open, its transcript:" << transcript;
            }
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
        }

        std::shared_ptr<ControlConnection> makeConnection(ControlConnectionOptions options = relaxedOptions())
        {
            return makeConnectionOn(pool_.get_executor(), options);
        }

        /**
         * @brief A connection on one thread, its handlers that are ready together run in the order they got ready.
         */
        std::shared_ptr<ControlConnection> makeSequentialConnection()
        {
            return makeConnectionOn(sequentialPool_.get_executor(), relaxedOptions());
        }

        std::shared_ptr<ControlConnection>
        makeConnectionOn(asio::any_io_executor executor, ControlConnectionOptions const& options)
        {
            auto connection = ControlConnection::create(std::move(executor), options);
            connection->onEnd(
                [ended = ended_](RemoteIo::RemoteError const& error)
                {
                    ended->set_value(error);
                }
            );
            connections_.push_back(connection);
            return connection;
        }

        std::vector<asio::ip::tcp::endpoint> endpoints() const
        {
            return {asio::ip::tcp::endpoint{asio::ip::make_address(server_->address()), server_->port()}};
        }

        /**
         * @brief Runs @p function, which returns an awaitable, on the strand of @p connection and waits for it.
         */
        template <typename FunctionT>
        auto run(std::shared_ptr<ControlConnection> const& connection, FunctionT function)
        {
            auto future = asio::co_spawn(connection->strand(), std::move(function), asio::use_future);
            if (future.wait_for(20s) != std::future_status::ready)
                throw std::runtime_error{"The coroutine did not end"};
            return future.get();
        }

        /**
         * @brief Connects @p connection and expects the 220 greeting.
         */
        void connect(std::shared_ptr<ControlConnection> const& connection)
        {
            const auto greeting =
                run(connection,
                    [connection, endpoints = endpoints()]() -> asio::awaitable<ReplyResult>
                    {
                        co_return co_await connection->connect(endpoints);
                    });
            expectReply(greeting, 220);
        }

        ReplyResult execute(std::shared_ptr<ControlConnection> const& connection, Command sent)
        {
            return run(
                connection,
                [connection, sent = std::move(sent)]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->execute(sent);
                }
            );
        }

        /**
         * @brief The error the connection ended with.
         */
        std::optional<RemoteIo::RemoteError> waitForEnd()
        {
            auto future = ended_->get_future();
            if (future.wait_for(20s) != std::future_status::ready)
                return std::nullopt;
            return future.get();
        }

        ControlState stateOf(std::shared_ptr<ControlConnection> const& connection)
        {
            return run(
                connection,
                [connection]() -> asio::awaitable<ControlState>
                {
                    co_return connection->state();
                }
            );
        }

        asio::thread_pool pool_{2};
        asio::thread_pool sequentialPool_{1};
        std::unique_ptr<ScriptedFtpServer> server_{};
        std::vector<std::shared_ptr<ControlConnection>> connections_{};
        std::shared_ptr<std::promise<RemoteIo::RemoteError>> ended_ =
            std::make_shared<std::promise<RemoteIo::RemoteError>>();
    };

    TEST_F(ControlConnectionTests, GreetingThenCommand)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").reply("200 Fine").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        EXPECT_EQ(connection->remoteEndpoint(), endpoints().front());
        expectReply(execute(connection, command(Verb::Noop)), 200);
        EXPECT_EQ(stateOf(connection), ControlState::Open);
    }

    TEST_F(ControlConnectionTests, MultiLineGreeting)
    {
        server_->addConnection(
            Script{}.replyRaw("220-Welcome\r\n220-You are user 1 of 50\r\n220 Ready\r\n").expectDisconnect()
        );
        const auto connection = makeConnection();
        const auto greeting =
            run(connection,
                [connection, endpoints = endpoints()]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->connect(endpoints);
                });
        ASSERT_TRUE(greeting) << greeting.error().toString();
        EXPECT_EQ(greeting->lines.size(), 3u);
    }

    TEST_F(ControlConnectionTests, DelayedGreetingWaitsForThe220)
    {
        server_->addConnection(
            Script{}.reply("120 Ready in a moment").delay(100ms).reply("220 Ready").expectDisconnect()
        );
        connect(makeConnection());
    }

    TEST_F(ControlConnectionTests, RefusingGreetingEndsTheConnection)
    {
        server_->addConnection(Script{}.reply("421 Too many connections from your address"));
        const auto connection = makeConnection();
        const auto greeting =
            run(connection,
                [connection, endpoints = endpoints()]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->connect(endpoints);
                });
        expectError(greeting, RemoteErrorKind::ConnectionLimit);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::ConnectionLimit);
    }

    TEST_F(ControlConnectionTests, NoAddressToConnectTo)
    {
        const auto connection = makeConnection();
        const auto greeting =
            run(connection,
                [connection]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->connect({});
                });
        expectError(greeting, RemoteErrorKind::NoConnection);
    }

    TEST_F(ControlConnectionTests, RefusedConnect)
    {
        asio::io_context context{};
        asio::ip::tcp::acceptor unused{context, asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), 0}};
        const auto closedPort = unused.local_endpoint();
        unused.close();

        const auto connection = makeConnection();
        const auto greeting =
            run(connection,
                [connection, closedPort]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->connect({closedPort});
                });
        expectError(greeting, RemoteErrorKind::NoConnection);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(ControlConnectionTests, RepliesGoToTheCommandsInOrder)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("SIZE", "first")
                .checkpoint("first-received")
                .awaitRelease("answer-first")
                .reply("213 1")
                .expect("SIZE", "second")
                .reply("213 2")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);

        auto [first, second] =
            run(connection,
                [connection]()
                    -> asio::awaitable<std::pair<std::shared_ptr<PendingCommand>, std::shared_ptr<PendingCommand>>>
                {
                    co_return std::pair{
                        connection->send(command(Verb::Size, "first")).value(),
                        connection->send(command(Verb::Size, "second")).value(),
                    };
                });
        // The second waits for the first reply to the first (RFC 959 5.4).
        ASSERT_TRUE(server_->waitForCheckpoint("first-received"));
        EXPECT_EQ(
            run(connection,
                [second]() -> asio::awaitable<PendingCommand::Stage>
                {
                    co_return second->stage();
                }),
            PendingCommand::Stage::Queued
        );
        server_->release("answer-first");

        const auto replies =
            run(connection,
                [first, second]() -> asio::awaitable<std::pair<ReplyResult, ReplyResult>>
                {
                    auto firstReply = co_await first->finalReply();
                    auto secondReply = co_await second->finalReply();
                    co_return std::pair{std::move(firstReply), std::move(secondReply)};
                });
        ASSERT_TRUE(replies.first);
        ASSERT_TRUE(replies.second);
        EXPECT_EQ(replies.first->lines.front(), "213 1");
        EXPECT_EQ(replies.second->lines.front(), "213 2");
    }

    TEST_F(ControlConnectionTests, PreliminaryRepliesComeBeforeTheFinalOne)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .replyRaw("150-Opening\r\n150 the data connection\r\n")
                .delay(50ms)
                .reply("226 Done")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto replies =
            run(connection,
                [connection]() -> asio::awaitable<std::vector<ReplyResult>>
                {
                    const auto pending = connection->send(command(Verb::Retrieve, "file")).value();
                    std::vector<ReplyResult> replies{};
                    replies.push_back(co_await pending->nextReply());
                    replies.push_back(co_await pending->nextReply());
                    replies.push_back(co_await pending->nextReply());
                    co_return replies;
                });
        ASSERT_NO_FATAL_FAILURE(expectReply(replies[0], 150));
        EXPECT_EQ(replies[0]->lines.size(), 2u);
        expectReply(replies[1], 226);
        expectError(replies[2], RemoteErrorKind::Failure);
    }

    TEST_F(ControlConnectionTests, AbortGoesBetweenATransferAndItsFinalReply)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .reply("150 Opening")
                .expect("ABOR")
                .replyRaw("426 Transfer aborted\r\n226 Abort successful\r\n")
                .expect("NOOP")
                .reply("200 Fine")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto codes =
            run(connection,
                [connection]() -> asio::awaitable<std::vector<int>>
                {
                    const auto retrieve = connection->send(command(Verb::Retrieve, "file")).value();
                    const auto opened = co_await retrieve->nextReply();
                    const auto abort = connection->send(command(Verb::Abort)).value();
                    // Anything else waits until the transfer completed (RFC 959 4.2).
                    const auto noop = connection->send(command(Verb::Noop)).value();
                    const auto queued = noop->stage() == PendingCommand::Stage::Queued;
                    const auto transferEnd = co_await retrieve->finalReply();
                    const auto abortReply = co_await abort->finalReply();
                    const auto noopReply = co_await noop->finalReply();
                    co_return std::vector<int>{
                        opened ? opened->code : 0,
                        queued ? 1 : 0,
                        transferEnd ? transferEnd->code : 0,
                        abortReply ? abortReply->code : 0,
                        noopReply ? noopReply->code : 0,
                    };
                });
        EXPECT_EQ(codes, (std::vector<int>{150, 1, 426, 226, 200}));
    }

    TEST_F(ControlConnectionTests, CommandsWaitForTheEndOfATransfer)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .reply("150 Opening")
                .awaitRelease("finish")
                .reply("226 Done")
                .expect("NOOP")
                .reply("200 Fine")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto stages =
            run(connection,
                [connection, server = server_.get()]() -> asio::awaitable<std::vector<int>>
                {
                    const auto retrieve = connection->send(command(Verb::Retrieve, "file")).value();
                    const auto opened = co_await retrieve->nextReply();
                    // Only ABOR, STAT and QUIT go between the 150 and the end of the transfer (RFC 959 4.2).
                    const auto noop = connection->send(command(Verb::Noop)).value();
                    const auto queued = noop->stage() == PendingCommand::Stage::Queued;
                    server->release("finish");
                    const auto transferEnd = co_await retrieve->finalReply();
                    const auto noopReply = co_await noop->finalReply();
                    co_return std::vector<int>{
                        opened ? opened->code : 0,
                        queued ? 1 : 0,
                        transferEnd ? transferEnd->code : 0,
                        noopReply ? noopReply->code : 0,
                    };
                });
        EXPECT_EQ(stages, (std::vector<int>{150, 1, 226, 200}));
    }

    TEST_F(ControlConnectionTests, StatWaitsForTheEndOfATransfer)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .reply("150 Opening")
                .awaitRelease("finish")
                .reply("226 Done")
                .expect("STAT")
                .reply("211 Connected")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto stages =
            run(connection,
                [connection, server = server_.get()]() -> asio::awaitable<std::vector<int>>
                {
                    const auto retrieve = connection->send(command(Verb::Retrieve, "file")).value();
                    const auto opened = co_await retrieve->nextReply();
                    // A server answers STAT at once, its reply would take the place of the 226.
                    const auto status = connection->send(command(Verb::Status)).value();
                    const auto queued = status->stage() == PendingCommand::Stage::Queued;
                    server->release("finish");
                    const auto transferEnd = co_await retrieve->finalReply();
                    const auto statusReply = co_await status->finalReply();
                    co_return std::vector<int>{
                        opened ? opened->code : 0,
                        queued ? 1 : 0,
                        transferEnd ? transferEnd->code : 0,
                        statusReply ? statusReply->code : 0,
                    };
                });
        EXPECT_EQ(stages, (std::vector<int>{150, 1, 226, 211}));
    }

    TEST_F(ControlConnectionTests, AbortGoesAheadOfCommandsQueuedBeforeIt)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .reply("150 Opening")
                .expect("ABOR")
                .replyRaw("426 Transfer aborted\r\n226 Abort successful\r\n")
                .expect("NOOP")
                .reply("200 Fine")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto codes =
            run(connection,
                [connection]() -> asio::awaitable<std::vector<int>>
                {
                    const auto retrieve = connection->send(command(Verb::Retrieve, "file")).value();
                    const auto opened = co_await retrieve->nextReply();
                    const auto noop = connection->send(command(Verb::Noop)).value();
                    const auto abort = connection->send(command(Verb::Abort)).value();
                    if (abort->stage() != PendingCommand::Stage::Written)
                        co_return std::vector<int>{opened ? opened->code : 0};
                    const auto transferEnd = co_await retrieve->finalReply();
                    const auto abortReply = co_await abort->finalReply();
                    const auto noopReply = co_await noop->finalReply();
                    co_return std::vector<int>{
                        opened ? opened->code : 0,
                        transferEnd ? transferEnd->code : 0,
                        abortReply ? abortReply->code : 0,
                        noopReply ? noopReply->code : 0,
                    };
                });
        EXPECT_EQ(codes, (std::vector<int>{150, 426, 226, 200}));
    }

    TEST_F(ControlConnectionTests, PreliminaryReplyNothingWaitsForIsDropped)
    {
        // The final reply before the 150, and a 150 after a failure, as some servers send them.
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "first")
                .replyRaw("226 Done\r\n150 Opening late\r\n")
                .expect("RETR", "second")
                .replyRaw("550 No such file\r\n150 Opening anyway\r\n")
                .expect("NOOP")
                .reply("200 Fine")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        expectReply(execute(connection, command(Verb::Retrieve, "first")), 226);
        expectReply(execute(connection, command(Verb::Retrieve, "second")), 550);
        expectReply(execute(connection, command(Verb::Noop)), 200);
        EXPECT_EQ(stateOf(connection), ControlState::Open);
    }

    TEST_F(ControlConnectionTests, ReplyNothingWaitsForEndsTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").delay(50ms).unsolicited("200 Surprise"));
        const auto connection = makeConnection();
        connect(connection);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::ProtocolViolation);
        EXPECT_EQ(ended->protocolCode, 200);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        expectError(execute(connection, command(Verb::Noop)), RemoteErrorKind::ProtocolViolation);
    }

    TEST_F(ControlConnectionTests, SecondGreetingEndsTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").reply("220 Ready again"));
        const auto connection = makeConnection();
        connect(connection);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::ProtocolViolation);
    }

    TEST_F(ControlConnectionTests, ExtraReplyAfterACommandEndsTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").replyRaw("200 Fine\r\n200 Fine again\r\n"));
        const auto connection = makeConnection();
        connect(connection);
        expectReply(execute(connection, command(Verb::Noop)), 200);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::ProtocolViolation);
    }

    TEST_F(ControlConnectionTests, IdleTimeoutOfTheServerIsSeenAtOnce)
    {
        server_->addConnection(Script{}.reply("220 Ready").delay(50ms).unsolicited("421 Timeout").closeControl());
        const auto connection = makeConnection();
        connect(connection);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(ended->protocolCode, 421);
    }

    TEST_F(ControlConnectionTests, ServiceClosingReplyIsTheReplyAndEndsTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").reply("421 Service closing").closeControl());
        const auto connection = makeConnection();
        connect(connection);
        expectReply(execute(connection, command(Verb::Noop)), 421);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::ConnectionLost);
    }

    TEST_F(ControlConnectionTests, ServerClosingAnIdleConnectionIsSeen)
    {
        server_->addConnection(Script{}.reply("220 Ready").delay(50ms).closeControl());
        const auto connection = makeConnection();
        connect(connection);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(ControlConnectionTests, ResetWhileWaitingForAReply)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").resetControl());
        const auto connection = makeConnection();
        connect(connection);
        expectError(execute(connection, command(Verb::Noop)), RemoteErrorKind::ConnectionLost);
    }

    TEST_F(ControlConnectionTests, CloseInsideAReply)
    {
        server_->addConnection(
            Script{}.reply("220 Ready").expect("FEAT").replyRaw("211-Features\r\n MDTM\r\n").closeControl()
        );
        const auto connection = makeConnection();
        connect(connection);
        expectError(execute(connection, command(Verb::Features)), RemoteErrorKind::ConnectionLost);
    }

    TEST_F(ControlConnectionTests, QuitClosesTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("QUIT").reply("221 Goodbye").closeControl());
        const auto connection = makeConnection();
        connect(connection);
        expectReply(execute(connection, command(Verb::Quit)), 221);
        EXPECT_EQ(stateOf(connection), ControlState::Closed);
        EXPECT_FALSE(connection->failure());
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteErrorKind::Cancelled);
    }

    TEST_F(ControlConnectionTests, CloseFailsTheWaitingCommand)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").checkpoint("received").stall());
        const auto connection = makeConnection();
        connect(connection);
        auto waiting = asio::co_spawn(connection->strand(), connection->execute(command(Verb::Noop)), asio::use_future);
        ASSERT_TRUE(server_->waitForCheckpoint("received"));
        connection->close();
        ASSERT_EQ(waiting.wait_for(20s), std::future_status::ready);
        expectError(waiting.get(), RemoteErrorKind::Cancelled);
        EXPECT_EQ(stateOf(connection), ControlState::Closed);
    }

    TEST_F(ControlConnectionTests, DroppingTheLastOwnerClosesTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").checkpoint("received").stall());
        // Not through makeConnection(), which keeps a copy for the end of the test.
        auto connection = ControlConnection::create(pool_.get_executor(), relaxedOptions());
        connect(connection);
        const auto pending =
            run(connection,
                [connection]() -> asio::awaitable<std::shared_ptr<PendingCommand>>
                {
                    co_return connection->send(command(Verb::Noop)).value();
                });
        ASSERT_TRUE(server_->waitForCheckpoint("received"));
        auto waiting = asio::co_spawn(
            connection->strand(),
            [pending]() -> asio::awaitable<ReplyResult>
            {
                co_return co_await pending->finalReply();
            },
            asio::use_future
        );

        connection.reset();
        EXPECT_TRUE(server_->waitForTranscriptEntry(TranscriptEntry::Kind::Disconnected));
        ASSERT_EQ(waiting.wait_for(20s), std::future_status::ready);
        expectError(waiting.get(), RemoteErrorKind::Cancelled);
    }

    TEST_F(ControlConnectionTests, WithdrawnCommandIsNeverWritten)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("SIZE", "first")
                .checkpoint("first-received")
                .awaitRelease("answer-first")
                .reply("213 1")
                .expect("NOOP")
                .reply("200 Fine")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto [first, second] =
            run(connection,
                [connection]()
                    -> asio::awaitable<std::pair<std::shared_ptr<PendingCommand>, std::shared_ptr<PendingCommand>>>
                {
                    co_return std::pair{
                        connection->send(command(Verb::Size, "first")).value(),
                        connection->send(command(Verb::Size, "second")).value(),
                    };
                });
        ASSERT_TRUE(server_->waitForCheckpoint("first-received"));
        const auto replies =
            run(connection,
                [connection, first, second, server = server_.get()]()
                    -> asio::awaitable<std::pair<ReplyResult, ReplyResult>>
                {
                    const auto withdrawn = connection->withdraw(*second);
                    server->release("answer-first");
                    auto secondReply = co_await second->finalReply();
                    auto firstReply = co_await first->finalReply();
                    co_return std::pair{
                        withdrawn ? std::move(firstReply) : ReplyResult{std::unexpected(RemoteIo::RemoteError{})},
                        std::move(secondReply),
                    };
                });
        expectReply(replies.first, 213);
        expectError(replies.second, RemoteErrorKind::Cancelled);
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(ControlConnectionTests, CancelledWaitKeepsTheConnectionInStep)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("SIZE", "slow")
                .checkpoint("received")
                .delay(300ms)
                .reply("213 1")
                .expect("NOOP")
                .reply("200 Fine")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto cancelled =
            run(connection,
                [connection]() -> asio::awaitable<ReplyResult>
                {
                    const auto [exception, reply] = co_await asio::co_spawn(
                        connection->strand(),
                        connection->execute(command(Verb::Size, "slow")),
                        asio::cancel_after(50ms, asio::as_tuple(asio::use_awaitable))
                    );
                    if (exception)
                        std::rethrow_exception(exception);
                    co_return reply;
                });
        expectError(cancelled, RemoteErrorKind::Cancelled);
        // The late 213 goes to the cancelled command, the NOOP gets its own reply.
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(ControlConnectionTests, OverlappingWaitsTakeTheRepliesInTurn)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .awaitRelease("answer")
                .reply("150 Opening")
                .reply("226 Done")
                .expectDisconnect()
        );
        const auto connection = makeSequentialConnection();
        connect(connection);
        const auto codes =
            run(connection,
                [connection, server = server_.get()]() -> asio::awaitable<std::vector<int>>
                {
                    const auto pending = connection->send(command(Verb::Retrieve, "file")).value();
                    // The strand is busy until both waits are in place, no reply can come before.
                    server->release("answer");
                    const auto [order, firstException, first, secondException, second] =
                        co_await asio::experimental::make_parallel_group(
                            asio::co_spawn(connection->strand(), pending->nextReply(), asio::deferred),
                            asio::co_spawn(connection->strand(), pending->nextReply(), asio::deferred)
                        )
                            .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
                    if (firstException)
                        std::rethrow_exception(firstException);
                    if (secondException)
                        std::rethrow_exception(secondException);
                    co_return std::vector<int>{first ? first->code : 0, second ? second->code : 0};
                });
        EXPECT_EQ(codes, (std::vector<int>{150, 226}));
    }

    TEST_F(ControlConnectionTests, WaitCancelledWhileAPreliminaryReplyIsHeldEndsCancelled)
    {
        // The 125 wakes both waits. The nextReply() takes it and the group cancels the finalReply() before that
        // resumes, with the 150 held.
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .awaitRelease("answer")
                .replyRaw("125 Data connection already open\r\n150 Opening\r\n")
                .expectDisconnect()
        );
        const auto connection = makeSequentialConnection();
        connect(connection);
        const auto replies =
            run(connection,
                [connection, server = server_.get()]() -> asio::awaitable<std::pair<ReplyResult, ReplyResult>>
                {
                    const auto pending = connection->send(command(Verb::Retrieve, "file")).value();
                    server->release("answer");
                    auto [order, nextException, nextResult, finalException, finalResult] =
                        co_await asio::experimental::make_parallel_group(
                            asio::co_spawn(connection->strand(), pending->nextReply(), asio::deferred),
                            asio::co_spawn(connection->strand(), pending->finalReply(), asio::deferred)
                        )
                            .async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);
                    if (nextException)
                        std::rethrow_exception(nextException);
                    if (finalException)
                        std::rethrow_exception(finalException);
                    co_return std::pair{std::move(nextResult), std::move(finalResult)};
                });
        expectReply(replies.first, 125);
        expectError(replies.second, RemoteErrorKind::Cancelled);
    }

    TEST_F(ControlConnectionTests, ArgumentThatCannotBeSentLeavesTheConnectionUsable)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").reply("200 Fine").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto refused = execute(connection, command(Verb::Delete, "a\r\nDELE b"));
        expectError(refused, RemoteErrorKind::Failure);
        EXPECT_EQ(refused.error().wrapperError, RemoteIo::WrapperErrors::InvalidPath);
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(ControlConnectionTests, BytesInAnyPieces)
    {
        server_->addConnection(
            Script{}
                .dribble("220-Ready\r\n220 Go\r\n", 2ms)
                .expect("FEAT")
                .dribble("211-Features:\r\n MDTM\r\n SIZE\r\n211 End\r\n", 1ms)
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto features = execute(connection, command(Verb::Features));
        ASSERT_NO_FATAL_FAILURE(expectReply(features, 211));
        EXPECT_EQ(features->lines.size(), 4u);
    }

    TEST_F(ControlConnectionTests, LineInsideAReplyWithAnotherCodeIsText)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("SITE", "HELP")
                .replyRaw("214-Commands:\r\n200 is a code\r\n214 End\r\n")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto help = execute(connection, command(Verb::Site, "HELP"));
        ASSERT_NO_FATAL_FAILURE(expectReply(help, 214));
        EXPECT_EQ(help->lines.size(), 3u);
    }

    TEST_F(ControlConnectionTests, ReplyEndingOnAnotherCodeSettlesAfterAQuietTime)
    {
        // glFTPd ends a multi-line reply on another code, nothing follows that line.
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("STAT")
                .replyRaw("211-Status:\r\n Connected\r\n200 End\r\n")
                .expectDisconnect()
        );
        auto options = relaxedOptions();
        options.settleTime = 50ms;
        const auto connection = makeConnection(options);
        connect(connection);
        const auto status = execute(connection, command(Verb::Status));
        ASSERT_NO_FATAL_FAILURE(expectReply(status, 200));
        EXPECT_EQ(status->lines.size(), 3u);
    }

    TEST_F(ControlConnectionTests, OverlongLineEndsTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").replyRaw("200 " + std::string(9000u, 'x')));
        const auto connection = makeConnection();
        connect(connection);
        expectError(execute(connection, command(Verb::Noop)), RemoteErrorKind::ProtocolViolation);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(ControlConnectionTests, LineWithoutACodeEndsTheConnection)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").reply("999 Unknown"));
        const auto connection = makeConnection();
        connect(connection);
        expectError(execute(connection, command(Verb::Noop)), RemoteErrorKind::ProtocolViolation);
    }

    TEST_F(ControlConnectionTests, GreetingNeverSent)
    {
        server_->addConnection(Script{}.stall());
        auto options = relaxedOptions();
        options.connectTimeout = 200ms;
        const auto connection = makeConnection(options);
        const auto greeting =
            run(connection,
                [connection, endpoints = endpoints()]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->connect(endpoints);
                });
        expectError(greeting, RemoteErrorKind::TimedOut);
    }

    TEST_F(ControlConnectionTests, DelayedGreetingStaysWithinTheConnectTimeout)
    {
        server_->addConnection(Script{}.reply("120 Ready in ten minutes").stall());
        auto options = relaxedOptions();
        options.connectTimeout = 200ms;
        const auto connection = makeConnection(options);
        const auto greeting =
            run(connection,
                [connection, endpoints = endpoints()]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->connect(endpoints);
                });
        expectError(greeting, RemoteErrorKind::TimedOut);
    }

    TEST_F(ControlConnectionTests, NoReplyWithinTheCommandTimeout)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").stall());
        auto options = relaxedOptions();
        options.commandTimeout = 200ms;
        const auto connection = makeConnection(options);
        connect(connection);
        expectError(execute(connection, command(Verb::Noop)), RemoteErrorKind::TimedOut);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(ControlConnectionTests, CommandTimeoutCoversTheWholeReply)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").dribble("200 Fine\r\n", 100ms));
        auto options = relaxedOptions();
        options.commandTimeout = 300ms;
        const auto connection = makeConnection(options);
        connect(connection);
        expectError(execute(connection, command(Verb::Noop)), RemoteErrorKind::TimedOut);
    }

    TEST_F(ControlConnectionTests, TransferWaitsForItsFinalReplyPastTheCommandTimeout)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("RETR", "file")
                .reply("150 Opening")
                .delay(400ms)
                .reply("226 Done")
                .expectDisconnect()
        );
        auto options = relaxedOptions();
        options.commandTimeout = 200ms;
        const auto connection = makeConnection(options);
        connect(connection);
        expectReply(execute(connection, command(Verb::Retrieve, "file")), 226);
    }

    TEST_F(ControlConnectionTests, LimitedFinalReplyOfATransfer)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("RETR", "file").reply("150 Opening").stall());
        const auto connection = makeConnection();
        connect(connection);
        const auto reply =
            run(connection,
                [connection]() -> asio::awaitable<ReplyResult>
                {
                    const auto pending = connection->send(command(Verb::Retrieve, "file")).value();
                    const auto opened = co_await pending->nextReply();
                    if (!opened)
                        co_return opened;
                    connection->limitFinalReply(*pending, 100ms);
                    co_return co_await pending->finalReply();
                });
        expectError(reply, RemoteErrorKind::TimedOut);
    }
}
