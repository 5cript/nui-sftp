#pragma once

#include "test_control_connection_tls.hpp"

#include <ftp/client/data_connection.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using DataResult = std::expected<std::shared_ptr<DataConnection>, RemoteIo::RemoteError>;
        using TextResult = std::expected<std::string, RemoteIo::RemoteError>;

        DataConnectionOptions quickDataOptions()
        {
            return DataConnectionOptions{.connectTimeout = 2s, .inactivityTimeout = 2s, .closeNotifyTimeout = 2s};
        }

        /**
         * @brief A port nothing listens on, the listener that had it is closed again.
         */
        std::uint16_t closedPort()
        {
            asio::io_context context{};
            asio::ip::tcp::acceptor acceptor{context, asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), 0u}};
            return acceptor.local_endpoint().port();
        }

        void expectText(TextResult const& result, std::string_view text)
        {
            ASSERT_TRUE(result) << result.error().toString();
            EXPECT_EQ(*result, text);
        }

        /**
         * @brief Reads @p data to its end.
         */
        asio::awaitable<TextResult> readToTheEnd(DataConnection& data)
        {
            std::string content{};
            std::array<char, 4096> buffer{};
            std::expected<std::size_t, RemoteIo::RemoteError> read = std::size_t{1u};
            while (read && *read > 0u)
            {
                read = co_await data.readSome(asio::buffer(buffer));
                if (read)
                    content.append(buffer.data(), *read);
            }
            if (!read)
                co_return std::unexpected(read.error());
            co_return content;
        }

        /**
         * @brief One read of @p data, which stays alive for it.
         */
        asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
        readOnce(std::shared_ptr<DataConnection> data)
        {
            std::array<char, 16> buffer{};
            co_return co_await data->readSome(asio::buffer(buffer));
        }

        /**
         * @brief An application data record whose content no key decrypts.
         */
        std::string damagedRecord()
        {
            std::string record{"\x17\x03\x03\x00\x10", 5u};
            record.append(16u, 'x');
            return record;
        }

        template <typename ValueT>
        void expectConnectionLost(std::expected<ValueT, RemoteIo::RemoteError> const& result)
        {
            ASSERT_FALSE(result) << "the operation on the failed stream succeeded";
            EXPECT_EQ(result.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << result.error().toString();
        }
    }

    class DataConnectionTests : public ControlConnectionTlsTests
    {
      protected:
        DataResult openData(
            std::shared_ptr<ControlConnection> const& connection,
            DataConnectionOptions options = quickDataOptions(),
            std::shared_ptr<PassiveChoice> choice = std::make_shared<PassiveChoice>()
        )
        {
            return run(
                connection,
                [connection, options, choice = std::move(choice)]() -> asio::awaitable<DataResult>
                {
                    co_return co_await DataConnection::open(connection, options, choice);
                }
            );
        }

        TextResult readAll(std::shared_ptr<ControlConnection> const& connection, std::shared_ptr<DataConnection> data)
        {
            return run(
                connection,
                [data = std::move(data)]() -> asio::awaitable<TextResult>
                {
                    co_return co_await readToTheEnd(*data);
                }
            );
        }

        std::vector<std::string> commandsOf(std::shared_ptr<ControlConnection> const& connection)
        {
            return commandsAfterDisconnect(connection);
        }
    };

    TEST_F(DataConnectionTests, ExtendedPassiveConnectsToTheControlPeer)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("EPSV")
                .passive(DataPlan{}.send("file content", 5u))
                .beginData()
                .awaitData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto data = openData(connection);
        ASSERT_TRUE(data) << data.error().toString();
        EXPECT_EQ((*data)->remoteEndpoint().address(), connection->remoteEndpoint().address());
        const auto content = readAll(connection, *data);
        ASSERT_TRUE(content) << content.error().toString();
        EXPECT_EQ(*content, "file content");
    }

    TEST_F(DataConnectionTests, RefusedExtendedPassiveFallsBackToPassiveForTheSession)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("EPSV")
                .reply("500 EPSV not understood")
                .expect("PASV")
                .passive(DataPlan{}.send("first"))
                .beginData()
                .awaitData()
                .expect("PASV")
                .passive(DataPlan{}.send("second"))
                .beginData()
                .awaitData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto choice = std::make_shared<PassiveChoice>();
        const auto first = openData(connection, quickDataOptions(), choice);
        ASSERT_TRUE(first) << first.error().toString();
        expectText(readAll(connection, *first), "first");
        EXPECT_TRUE(choice->extendedPassiveFailed());
        const auto second = openData(connection, quickDataOptions(), choice);
        ASSERT_TRUE(second) << second.error().toString();
        expectText(readAll(connection, *second), "second");
        EXPECT_EQ(commandsOf(connection), (std::vector<std::string>{"EPSV", "PASV", "PASV"}));
    }

    TEST_F(DataConnectionTests, ExtendedPassivePortThatCannotBeConnectedFallsBackToPassive)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("EPSV")
                .passive(DataPlan{}.announce("127.0.0.1", closedPort()))
                .expect("PASV")
                .passive(DataPlan{}.send("through PASV"))
                .beginData()
                .awaitData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto choice = std::make_shared<PassiveChoice>();
        const auto data = openData(connection, quickDataOptions(), choice);
        ASSERT_TRUE(data) << data.error().toString();
        expectText(readAll(connection, *data), "through PASV");
        EXPECT_TRUE(choice->extendedPassiveFailed());
    }

    TEST_F(DataConnectionTests, AnnouncedPassiveAddressIsIgnored)
    {
        // RFC 2577 4: a server could send the client to another host.
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("PASV")
                .passive(DataPlan{}.announceAs("227 Entering Passive Mode (203,0,113,7,{high},{low})."))
                .beginData()
                .awaitData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.extendedPassive = ExtendedPassive::Never;
        const auto data = openData(connection, options);
        ASSERT_TRUE(data) << data.error().toString();
        EXPECT_EQ((*data)->remoteEndpoint().address(), connection->remoteEndpoint().address());
        expectText(readAll(connection, *data), "");
    }

    TEST_F(DataConnectionTests, TrustedPassiveAddressIsUsed)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("PASV")
                .passive(DataPlan{}.announceAs("227 Entering Passive Mode (203,0,113,7,{high},{low})."))
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.extendedPassive = ExtendedPassive::Never;
        options.trustPassiveAddress = true;
        options.connectTimeout = 300ms;
        const auto data = openData(connection, options);
        // 203.0.113.7 is a documentation address, nothing answers there.
        ASSERT_FALSE(data);
        EXPECT_TRUE(
            data.error().kind == RemoteIo::RemoteErrorKind::TimedOut ||
            data.error().kind == RemoteIo::RemoteErrorKind::NoConnection
        ) << data.error().toString();
    }

    TEST_F(DataConnectionTests, NeverSendsPassiveAlone)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("PASV")
                .passive(DataPlan{}.send("x"))
                .beginData()
                .awaitData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.extendedPassive = ExtendedPassive::Never;
        const auto data = openData(connection, options);
        ASSERT_TRUE(data) << data.error().toString();
        expectText(readAll(connection, *data), "x");
    }

    TEST_F(DataConnectionTests, AlwaysDoesNotFallBack)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("EPSV").reply("502 No").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.extendedPassive = ExtendedPassive::Always;
        const auto data = openData(connection, options);
        ASSERT_FALSE(data);
        EXPECT_EQ(data.error().protocolCode, 502);
        EXPECT_EQ(commandsOf(connection), (std::vector<std::string>{"EPSV"}));
    }

    TEST_F(DataConnectionTests, IpVersion6UsesExtendedPassiveOnly)
    {
        auto started = ScriptedFtpServer::start({.ipv6 = true});
        if (!started)
            GTEST_SKIP() << "No IPv6 loopback: " << started.error();
        server_ = std::move(started).value();
        server_->addConnection(Script{}.reply("220 Ready").expect("EPSV").reply("500 No").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto data = openData(connection);
        ASSERT_FALSE(data);
        EXPECT_EQ(commandsOf(connection), (std::vector<std::string>{"EPSV"}));
    }

    TEST_F(DataConnectionTests, NeverOnIpVersion6IsUnsupported)
    {
        auto started = ScriptedFtpServer::start({.ipv6 = true});
        if (!started)
            GTEST_SKIP() << "No IPv6 loopback: " << started.error();
        server_ = std::move(started).value();
        server_->addConnection(Script{}.reply("220 Ready").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.extendedPassive = ExtendedPassive::Never;
        const auto data = openData(connection, options);
        ASSERT_FALSE(data);
        EXPECT_EQ(data.error().kind, RemoteIo::RemoteErrorKind::Unsupported) << data.error().toString();
        EXPECT_TRUE(commandsOf(connection).empty());
    }

    TEST_F(DataConnectionTests, SilentDataTimesOut)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("EPSV")
                .passive(DataPlan{}.stallAfter("ab"))
                .beginData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.inactivityTimeout = 300ms;
        const auto data = openData(connection, options);
        ASSERT_TRUE(data) << data.error().toString();
        const auto content = readAll(connection, *data);
        ASSERT_FALSE(content);
        EXPECT_EQ(content.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << content.error().toString();
    }

    TEST_F(DataConnectionTests, ResetDataIsConnectionLost)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("EPSV")
                .passive(DataPlan{}.send("ab").reset())
                // A reset before the client saw its connect complete would fail the connect instead.
                .awaitRelease("connected")
                .beginData()
                .awaitData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto data = openData(connection);
        ASSERT_TRUE(data) << data.error().toString();
        server_->release("connected");
        const auto content = readAll(connection, *data);
        ASSERT_FALSE(content);
        EXPECT_EQ(content.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << content.error().toString();
    }

    TEST_F(DataConnectionTests, UploadEndsOnceTheServerTookIt)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("EPSV")
                .passive(DataPlan{}.expectUpload(1u << 20u))
                .beginData()
                .awaitData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto data = openData(connection);
        ASSERT_TRUE(data) << data.error().toString();
        const std::string content(256u * 1024u, 'u');
        const auto finished =
            run(connection,
                [data = *data, &content]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    if (auto written = co_await data->writeAll(asio::buffer(content)); !written)
                        co_return written;
                    co_return co_await data->finishUpload();
                });
        ASSERT_TRUE(finished) << finished.error().toString();
        connection->close();
        ASSERT_TRUE(server_->waitUntilDisconnected());
        const auto transcript = server_->transcript();
        const auto uploaded = std::ranges::find_if(
            transcript,
            [](TranscriptEntry const& entry)
            {
                return entry.kind == TranscriptEntry::Kind::DataUploaded;
            }
        );
        ASSERT_NE(uploaded, transcript.end());
        EXPECT_EQ(uploaded->detail, content);
    }

    TEST_F(DataConnectionTests, UploadWhoseDataConnectionTheServerKeepsOpenTimesOut)
    {
        // The server read to the end but never ends its side, nothing tells the client that it took the bytes.
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("EPSV")
                .passive(DataPlan{}.expectUpload(1024u).hold())
                .beginData()
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.inactivityTimeout = 300ms;
        const auto data = openData(connection, options);
        ASSERT_TRUE(data) << data.error().toString();
        const auto finished =
            run(connection,
                [data = *data]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    const std::string_view content = "small";
                    if (auto written = co_await data->writeAll(asio::buffer(content)); !written)
                        co_return written;
                    co_return co_await data->finishUpload();
                });
        ASSERT_FALSE(finished) << "the upload ended without the server's end";
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << finished.error().toString();
    }

    TEST_F(DataConnectionTests, CloseEndsAReadAsCancelled)
    {
        server_->addConnection(
            Script{}.reply("220 Ready").expect("EPSV").passive(DataPlan{}.stallAfter("")).beginData().expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        auto options = quickDataOptions();
        options.inactivityTimeout = 20s;
        const auto data = openData(connection, options);
        ASSERT_TRUE(data) << data.error().toString();
        auto reading = asio::co_spawn(connection->strand(), readToTheEnd(**data), asio::use_future);
        ASSERT_TRUE(server_->waitForTranscriptEntry(TranscriptEntry::Kind::DataConnected));
        asio::post(
            connection->strand(),
            [data = *data]()
            {
                data->close();
            }
        );
        ASSERT_EQ(reading.wait_for(20s), std::future_status::ready);
        const auto content = reading.get();
        ASSERT_FALSE(content);
        EXPECT_EQ(content.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << content.error().toString();
    }

    class DataConnectionTlsTests : public DataConnectionTests
    {
      protected:
        /**
         * @brief Secures @p data like a transfer of @p connection does: the control session and its leaf.
         */
        std::expected<void, RemoteIo::RemoteError> secure(
            std::shared_ptr<ControlConnection> const& connection,
            std::shared_ptr<DataConnection> const& data,
            TlsConnectionSettings tls,
            bool resume
        )
        {
            return run(
                connection,
                [connection, data, tls = std::move(tls), resume]()
                    -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    auto session = resume ? connection->sessionForData() : std::nullopt;
                    co_return co_await data->secure(
                        tls, std::move(session), connection->trustedCertificate()->certificate.certificateSha256
                    );
                }
            );
        }

        /**
         * @brief A TLS control connection with a NOOP behind the handshake, so its tickets were read.
         */
        std::shared_ptr<ControlConnection> securedConnection(TlsConnectionSettings const& tls)
        {
            const auto connection = makeConnection();
            connect(connection);
            const auto trusted = startTls(connection, tls);
            EXPECT_TRUE(trusted) << trusted.error().toString();
            expectReply(execute(connection, command(Verb::Noop)), 200);
            return connection;
        }

        Script securedScript(DataPlan plan)
        {
            auto script = explicitTls();
            script.expect("NOOP").reply("200 Fine").expect("EPSV").passive(std::move(plan)).beginData();
            return script;
        }

        /**
         * @brief Runs @p function like run(), but closes @p data where it does not end, which ends a loop on it.
         */
        template <typename FunctionT>
        auto runOn(
            std::shared_ptr<ControlConnection> const& connection,
            std::shared_ptr<DataConnection> const& data,
            FunctionT function
        )
        {
            auto future = asio::co_spawn(connection->strand(), std::move(function), asio::use_future);
            if (future.wait_for(20s) != std::future_status::ready)
            {
                ADD_FAILURE() << "The operation on the data connection did not end";
                asio::post(
                    connection->strand(),
                    [data]()
                    {
                        data->close();
                    }
                );
            }
            return future.get();
        }

        std::expected<std::size_t, RemoteIo::RemoteError>
        readFrom(std::shared_ptr<ControlConnection> const& connection, std::shared_ptr<DataConnection> const& data)
        {
            return runOn(connection, data, readOnce(data));
        }

        std::expected<void, RemoteIo::RemoteError>
        writeTo(std::shared_ptr<ControlConnection> const& connection, std::shared_ptr<DataConnection> const& data)
        {
            return runOn(
                connection,
                data,
                [data]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    co_return co_await data->writeAll(asio::buffer(std::string_view{"upload"}));
                }
            );
        }

        std::expected<void, RemoteIo::RemoteError> finishUploadOn(
            std::shared_ptr<ControlConnection> const& connection,
            std::shared_ptr<DataConnection> const& data
        )
        {
            return runOn(
                connection,
                data,
                [data]() -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    co_return co_await data->finishUpload();
                }
            );
        }

        /**
         * @brief A secured data connection whose stream failed: the server sent a damaged record, the first read
         *        reported it.
         */
        std::shared_ptr<DataConnection> failedData(std::shared_ptr<ControlConnection> const& connection)
        {
            const auto opened = openData(connection);
            if (!opened)
            {
                ADD_FAILURE() << opened.error().toString();
                return nullptr;
            }
            auto const data = *opened;
            const auto secured = secure(connection, data, settings(), true);
            if (!secured)
            {
                ADD_FAILURE() << secured.error().toString();
                return nullptr;
            }
            expectConnectionLost(readFrom(connection, data));
            return data;
        }

        Script damagedRecordScript()
        {
            return securedScript(DataPlan{}.tls().sendBeneathTls(damagedRecord()).stallAfter("")).expectDisconnect();
        }
    };

    TEST_F(DataConnectionTlsTests, DataConnectionResumesTheControlSession)
    {
        server_->addConnection(
            securedScript(DataPlan{}.tls({.requireResumption = true}).send("secret")).awaitData().expectDisconnect()
        );
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto data = openData(connection);
        ASSERT_TRUE(data) << data.error().toString();
        const auto secured = secure(connection, *data, tls, true);
        ASSERT_TRUE(secured) << secured.error().toString();
        EXPECT_TRUE((*data)->resumed());
        expectText(readAll(connection, *data), "secret");
        EXPECT_FALSE((*data)->endedWithoutCloseNotify());
    }

    TEST_F(DataConnectionTlsTests, FullHandshakeNeedsTheControlCertificate)
    {
        server_->addConnection(securedScript(DataPlan{}.tls().send("secret")).awaitData().expectDisconnect());
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto data = openData(connection);
        ASSERT_TRUE(data) << data.error().toString();
        const auto secured = secure(connection, *data, tls, false);
        ASSERT_TRUE(secured) << secured.error().toString();
        EXPECT_FALSE((*data)->resumed());
        expectText(readAll(connection, *data), "secret");
    }

    TEST_F(DataConnectionTlsTests, CloseOnceAnsweredKeepsASilentConnectionOpen)
    {
        // The server says nothing on the data connection, like one that is still in its handshake.
        server_->addConnection(securedScript(DataPlan{}.tls().stallAfter("")).expectDisconnect());
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto opened = openData(connection);
        ASSERT_TRUE(opened) << opened.error().toString();
        auto const data = *opened;
        const auto secured = secure(connection, data, tls, true);
        ASSERT_TRUE(secured) << secured.error().toString();
        const auto stillOpen =
            run(connection,
                [data]() -> asio::awaitable<bool>
                {
                    data->closeOnceAnswered();
                    // Asked again, it goes on with the wait it has.
                    data->closeOnceAnswered();
                    const auto open = data->serverMayBeInHandshake();
                    data->close();
                    co_return open;
                });
        EXPECT_TRUE(stillOpen);
    }

    TEST_F(DataConnectionTlsTests, CloseOnceAnsweredClosesWithTheFirstBytesOfTheServer)
    {
        // The server only learns of the close when it comes: its plan waits for the client behind its bytes.
        server_->addConnection(securedScript(DataPlan{}.tls().stallAfter("answer")).expectDisconnect());
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto opened = openData(connection);
        ASSERT_TRUE(opened) << opened.error().toString();
        auto const data = *opened;
        const auto secured = secure(connection, data, tls, true);
        ASSERT_TRUE(secured) << secured.error().toString();
        run(connection,
            [data]() -> asio::awaitable<void>
            {
                data->closeOnceAnswered();
                co_return;
            });
        EXPECT_TRUE(server_->waitForTranscriptEntry(TranscriptEntry::Kind::DataEnded));
    }

    TEST_F(DataConnectionTlsTests, CloseOnceAnsweredEndsAReadUnderWayWithTheAnswer)
    {
        server_->addConnection(
            securedScript(DataPlan{}.tls().stallAfter("")).awaitRelease("answer").stopData().expectDisconnect()
        );
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto opened = openData(connection);
        ASSERT_TRUE(opened) << opened.error().toString();
        auto const data = *opened;
        const auto secured = secure(connection, data, tls, true);
        ASSERT_TRUE(secured) << secured.error().toString();
        // On the strand the read is under way before the next coroutine starts.
        auto reading = asio::co_spawn(connection->strand(), readOnce(data), asio::use_future);
        const auto stillOpen =
            run(connection,
                [data]() -> asio::awaitable<bool>
                {
                    data->closeOnceAnswered();
                    co_return data->serverMayBeInHandshake();
                });
        EXPECT_TRUE(stillOpen);
        server_->release("answer");
        ASSERT_EQ(reading.wait_for(20s), std::future_status::ready);
        const auto read = reading.get();
        ASSERT_FALSE(read);
        EXPECT_EQ(read.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << read.error().toString();
    }

    TEST_F(DataConnectionTlsTests, AnotherCertificateOnTheDataConnectionIsRefused)
    {
        // Signed by the same authority for the same names, still not the control connection's server.
        server_->addConnection(
            securedScript(DataPlan{}.tls({.refuseResumption = true, .certificate = tlsTestPki().otherServer}))
                .expectDisconnect()
        );
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto data = openData(connection);
        ASSERT_TRUE(data) << data.error().toString();
        const auto secured = secure(connection, *data, tls, true);
        ASSERT_FALSE(secured);
        EXPECT_EQ(secured.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << secured.error().toString();
        (*data)->close();
    }

    TEST_F(DataConnectionTlsTests, DataWithoutCloseNotifyIsMarked)
    {
        server_->addConnection(
            securedScript(DataPlan{}.tls({.closeWithoutCloseNotify = true}).send("cut")).awaitData().expectDisconnect()
        );
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto data = openData(connection);
        ASSERT_TRUE(data) << data.error().toString();
        ASSERT_TRUE(secure(connection, *data, tls, true));
        expectText(readAll(connection, *data), "cut");
        EXPECT_TRUE((*data)->endedWithoutCloseNotify());
    }

    TEST_F(DataConnectionTlsTests, ReadBehindAFailedReadFails)
    {
        server_->addConnection(damagedRecordScript());
        const auto connection = securedConnection(settings());
        const auto data = failedData(connection);
        ASSERT_TRUE(data);
        expectConnectionLost(readFrom(connection, data));
    }

    TEST_F(DataConnectionTlsTests, WriteBehindAFailedReadFails)
    {
        server_->addConnection(damagedRecordScript());
        const auto connection = securedConnection(settings());
        const auto data = failedData(connection);
        ASSERT_TRUE(data);
        expectConnectionLost(writeTo(connection, data));
    }

    TEST_F(DataConnectionTlsTests, UploadEndBehindAFailedReadFails)
    {
        server_->addConnection(damagedRecordScript());
        const auto connection = securedConnection(settings());
        const auto data = failedData(connection);
        ASSERT_TRUE(data);
        expectConnectionLost(finishUploadOn(connection, data));
    }

    TEST_F(DataConnectionTlsTests, EveryOperationBehindAFailedHandshakeFails)
    {
        server_->addConnection(securedScript(DataPlan{}.tls({.insteadOfHandshake = fatalAlert()})).expectDisconnect());
        const auto tls = settings();
        const auto connection = securedConnection(tls);
        const auto opened = openData(connection);
        ASSERT_TRUE(opened) << opened.error().toString();
        auto const data = *opened;
        ASSERT_FALSE(secure(connection, data, tls, true));
        expectConnectionLost(readFrom(connection, data));
        expectConnectionLost(writeTo(connection, data));
        // The end of the upload closes the data connection, so it goes last.
        expectConnectionLost(finishUploadOn(connection, data));
    }
}
