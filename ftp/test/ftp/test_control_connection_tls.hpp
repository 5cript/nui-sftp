#pragma once

#include "test_control_connection.hpp"

#include <ftp/client/tls.hpp>
#include <ftp/support/test_pki.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using TrustResult = std::expected<TrustedCertificate, RemoteIo::RemoteError>;

        TestPki const& tlsTestPki()
        {
            const auto made = testPki();
            if (!made)
                throw std::runtime_error{made.error()};
            return made->get();
        }

        Sha256Digest publicKeyDigestOf(CertifiedKey const& certified)
        {
            const std::unique_ptr<BIO, decltype(&BIO_free)> bio{
                BIO_new_mem_buf(certified.certificate.data(), static_cast<int>(certified.certificate.size())), &BIO_free
            };
            const std::unique_ptr<X509, decltype(&X509_free)> certificate{
                PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free
            };
            const auto info = describeCertificate(certificate.get());
            if (!info)
                throw std::runtime_error{"The test certificate cannot be read"};
            return info->publicKeySha256;
        }

        /**
         * @brief Greets, accepts AUTH TLS and starts TLS with @p behaviour.
         */
        Script explicitTls(TlsBehaviour behaviour = {})
        {
            Script script{};
            script.reply("220 Ready").expect("AUTH", "TLS").reply("234 Proceed").startTls(std::move(behaviour));
            return script;
        }

        bool
        hasEntry(std::vector<TranscriptEntry> const& transcript, TranscriptEntry::Kind kind, std::string_view detail)
        {
            return std::ranges::any_of(
                transcript,
                [kind, detail](TranscriptEntry const& entry)
                {
                    return entry.kind == kind && entry.detail.find(detail) != std::string::npos;
                }
            );
        }
    }

    class ControlConnectionTlsTests : public ControlConnectionTests
    {
      protected:
        void restartServer(ScriptedFtpServer::Options options)
        {
            server_.reset();
            auto started = ScriptedFtpServer::start(std::move(options));
            ASSERT_TRUE(started) << started.error();
            server_ = std::move(started).value();
        }

        TlsConnectionSettings
        settings(std::optional<Sha256Digest> pin = std::nullopt, std::string serverName = "127.0.0.1") const
        {
            auto context = TlsContext::create({.systemAuthorities = false, .authorities = {tlsTestPki().authority}});
            if (!context)
                throw std::runtime_error{context.error().toString()};
            return TlsConnectionSettings{
                .context = std::move(context).value(),
                .serverName = std::move(serverName),
                .pinnedPublicKey = pin,
            };
        }

        TrustResult startTls(std::shared_ptr<ControlConnection> const& connection, TlsConnectionSettings tls)
        {
            return run(
                connection,
                [connection, tls = std::move(tls)]() -> asio::awaitable<TrustResult>
                {
                    co_return co_await connection->startTls(tls);
                }
            );
        }

        ReplyResult
        connectWithImplicitTls(std::shared_ptr<ControlConnection> const& connection, TlsConnectionSettings tls)
        {
            return run(
                connection,
                [connection, endpoints = endpoints(), tls = std::move(tls)]() -> asio::awaitable<ReplyResult>
                {
                    co_return co_await connection->connect(endpoints, tls);
                }
            );
        }

        /**
         * @brief The commands of the first control connection once the client is gone.
         */
        std::vector<std::string> commandsAfterDisconnect(std::shared_ptr<ControlConnection> const& connection)
        {
            connection->close();
            EXPECT_TRUE(server_->waitUntilDisconnected());
            return server_->commands();
        }
    };

    TEST_F(ControlConnectionTlsTests, CommandsAfterAuthTlsGoOverTls)
    {
        server_->addConnection(explicitTls().expect("NOOP").reply("200 Fine").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_TRUE(trusted) << trusted.error().toString();
        EXPECT_EQ(trusted->source, TrustSource::Authorities);
        EXPECT_EQ(trusted->certificate.publicKeySha256, publicKeyDigestOf(tlsTestPki().server));
        ASSERT_TRUE(connection->trustedCertificate());
        EXPECT_EQ(connection->trustedCertificate()->source, TrustSource::Authorities);

        expectReply(execute(connection, command(Verb::Noop)), 200);
        const auto transcript = server_->transcript();
        const auto established = std::ranges::find_if(
            transcript,
            [](TranscriptEntry const& entry)
            {
                return entry.kind == TranscriptEntry::Kind::TlsEstablished;
            }
        );
        const auto noop = std::ranges::find_if(
            transcript,
            [](TranscriptEntry const& entry)
            {
                return entry.kind == TranscriptEntry::Kind::Command && entry.detail == "NOOP";
            }
        );
        ASSERT_NE(established, transcript.end());
        ASSERT_NE(noop, transcript.end());
        EXPECT_LT(established, noop);
    }

    TEST_F(ControlConnectionTlsTests, AuthAnsweredWith334StartsTls)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("AUTH", "TLS")
            .reply("334 Go ahead")
            .startTls()
            .expect("NOOP")
            .reply("200 Fine");
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_TRUE(trusted) << trusted.error().toString();
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(ControlConnectionTlsTests, RefusedAuthEndsTheConnection)
    {
        server_->addConnection(
            Script{}.reply("220 Ready").expect("AUTH", "TLS").reply("502 Not implemented").expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(trusted.error().protocolCode, 502);
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        expectError(execute(connection, command(Verb::Noop)), RemoteIo::RemoteErrorKind::NoSecureConnection);
        EXPECT_FALSE(connection->trustedCertificate());
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    TEST_F(ControlConnectionTlsTests, CancelledStartTlsEndsTheConnection)
    {
        // A 234 after the cancel would otherwise reach a reader that goes on in plaintext.
        server_->addConnection(Script{}.reply("220 Ready").expect("AUTH", "TLS").checkpoint("received").stall());
        const auto connection = makeConnection();
        connect(connection);
        const auto signal = std::make_shared<asio::cancellation_signal>();
        auto starting = asio::co_spawn(
            connection->strand(),
            connection->startTls(settings()),
            asio::bind_cancellation_slot(signal->slot(), asio::use_future)
        );
        ASSERT_TRUE(server_->waitForCheckpoint("received"));
        asio::post(
            connection->strand(),
            [signal]()
            {
                signal->emit(asio::cancellation_type::terminal);
            }
        );
        ASSERT_EQ(starting.wait_for(20s), std::future_status::ready);
        const auto trusted = starting.get();
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        expectError(execute(connection, command(Verb::Noop)), RemoteIo::RemoteErrorKind::Cancelled);
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    TEST_F(ControlConnectionTlsTests, AuthTlsOnABusyConnectionEndsIt)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").checkpoint("received").stall());
        const auto connection = makeConnection();
        connect(connection);
        const auto sent =
            run(connection,
                [connection]() -> asio::awaitable<bool>
                {
                    co_return connection->send(command(Verb::Noop)).has_value();
                });
        ASSERT_TRUE(sent);
        ASSERT_TRUE(server_->waitForCheckpoint("received"));
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"NOOP"}));
    }

    TEST_F(ControlConnectionTlsTests, ReplyBehindTheAuthReplyEndsTheConnection)
    {
        // The 200 could have been injected into the plaintext before the handshake.
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("AUTH", "TLS")
                .replyRaw("234 Proceed\r\n200 Injected\r\n")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    TEST_F(ControlConnectionTlsTests, PartOfALineBehindTheAuthReplyEndsTheConnection)
    {
        server_->addConnection(
            Script{}.reply("220 Ready").expect("AUTH", "TLS").replyRaw("234 Proceed\r\n2").expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    TEST_F(ControlConnectionTlsTests, PlaintextBeforeTheHandshakeEndsTheConnection)
    {
        // Read with the 234 or by the handshake, either way nothing else is sent.
        server_->addConnection(explicitTls({.plaintextFirst = "200 Injected\r\n"}).expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
        EXPECT_FALSE(hasEntry(server_->transcript(), TranscriptEntry::Kind::TlsEstablished, "control"));
    }

    struct UntrustedCase
    {
        std::string_view name{};
        CertifiedKey TestPki::* certificate{};
    };

    void PrintTo(UntrustedCase const& untrusted, std::ostream* stream)
    {
        *stream << untrusted.name;
    }

    class ControlConnectionUntrustedTests
        : public ControlConnectionTlsTests
        , public ::testing::WithParamInterface<UntrustedCase>
    {
      protected:
        CertifiedKey const& certificate() const
        {
            return tlsTestPki().*GetParam().certificate;
        }
    };

    TEST_P(ControlConnectionUntrustedTests, CertificateNeedsTrustAndNothingIsSent)
    {
        server_->addConnection(explicitTls({.certificate = certificate()}).expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::TrustRequired) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        const auto untrusted = connection->untrustedCertificate();
        ASSERT_TRUE(untrusted);
        EXPECT_EQ(untrusted->publicKeySha256, publicKeyDigestOf(certificate()));
        EXPECT_FALSE(untrusted->verifyFailure.empty());
        EXPECT_FALSE(connection->trustedCertificate());
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    TEST_P(ControlConnectionUntrustedTests, PinnedPublicKeyIsTrusted)
    {
        server_->addConnection(
            explicitTls({.certificate = certificate()}).expect("NOOP").reply("200 Fine").expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings(publicKeyDigestOf(certificate())));
        ASSERT_TRUE(trusted) << trusted.error().toString();
        EXPECT_EQ(trusted->source, TrustSource::Pin);
        EXPECT_FALSE(trusted->certificate.verifyFailure.empty());
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_P(ControlConnectionUntrustedTests, PinOfAnotherKeyDoesNotHelp)
    {
        server_->addConnection(explicitTls({.certificate = certificate()}).expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings(publicKeyDigestOf(tlsTestPki().otherServer)));
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::TrustRequired) << trusted.error().toString();
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    INSTANTIATE_TEST_SUITE_P(
        Certificates,
        ControlConnectionUntrustedTests,
        ::testing::Values(
            UntrustedCase{.name = "SelfSigned", .certificate = &TestPki::selfSigned},
            UntrustedCase{.name = "Expired", .certificate = &TestPki::expired},
            UntrustedCase{.name = "WrongName", .certificate = &TestPki::wrongName},
            UntrustedCase{.name = "OtherAuthority", .certificate = &TestPki::otherAuthority}
        ),
        [](::testing::TestParamInfo<UntrustedCase> const& info)
        {
            return std::string{info.param.name};
        }
    );

    TEST_F(ControlConnectionTlsTests, PinDoesNotReplaceTheAuthorities)
    {
        server_->addConnection(explicitTls().expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings(publicKeyDigestOf(tlsTestPki().otherServer)));
        ASSERT_TRUE(trusted) << trusted.error().toString();
        EXPECT_EQ(trusted->source, TrustSource::Authorities);
    }

    TEST_F(ControlConnectionTlsTests, CertificateHasToNameTheServer)
    {
        server_->addConnection(explicitTls().expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings(std::nullopt, "ftp.example.org"));
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::TrustRequired) << trusted.error().toString();
    }

    TEST_F(ControlConnectionTlsTests, DnsNameOfTheCertificateIsTrusted)
    {
        server_->addConnection(explicitTls().expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings(std::nullopt, "localhost"));
        ASSERT_TRUE(trusted) << trusted.error().toString();
        EXPECT_EQ(trusted->source, TrustSource::Authorities);
    }

    /**
     * @brief Bytes that replace the handshake end the connection. Read with the 234 they are bytes behind it, read by
     *        the handshake they fail it, or the server's close comes first.
     */
    class ControlConnectionHandshakeReplacedTests
        : public ControlConnectionTlsTests
        , public ::testing::WithParamInterface<std::string>
    {};

    TEST_P(ControlConnectionHandshakeReplacedTests, ConnectionEndsAndNothingIsSent)
    {
        // The server does not read on, a ClientHello read as lines can hold a CRLF.
        server_->addConnection(explicitTls({.insteadOfHandshake = GetParam()}).closeControl());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
        EXPECT_FALSE(connection->untrustedCertificate());
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    INSTANTIATE_TEST_SUITE_P(
        ServerBytes,
        ControlConnectionHandshakeReplacedTests,
        ::testing::Values(fatalAlert(), std::string(64u, '\x7f')),
        [](::testing::TestParamInfo<std::string> const& info)
        {
            return info.index == 0u ? std::string{"Alert"} : std::string{"Garbage"};
        }
    );

    TEST_F(ControlConnectionTlsTests, HandshakeWithoutAnAnswerTimesOut)
    {
        server_->addConnection(Script{}.reply("220 Ready").expect("AUTH", "TLS").reply("234 Proceed").stall());
        auto options = relaxedOptions();
        options.commandTimeout = 300ms;
        const auto connection = makeConnection(options);
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(ControlConnectionTlsTests, TlsBeforeVersion12IsRefused)
    {
        server_->addConnection(explicitTls({.maximumVersion = TLS1_1_VERSION}).expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_FALSE(trusted);
        EXPECT_EQ(trusted.error().kind, RemoteIo::RemoteErrorKind::NoSecureConnection) << trusted.error().toString();
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(ControlConnectionTlsTests, CommandsCannotBeSentWhileTlsStarts)
    {
        server_->addConnection(explicitTls().expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        auto starting = asio::co_spawn(connection->strand(), connection->startTls(settings()), asio::use_future);
        // Runs on the strand behind the start of startTls(), which waits for the 234 by then.
        const auto refused =
            run(connection,
                [connection]() -> asio::awaitable<std::pair<bool, bool>>
                {
                    co_return std::pair{
                        !connection->send(command(Verb::Noop)).has_value(),
                        !connection->send(command(Verb::Authenticate, "TLS")).has_value(),
                    };
                });
        EXPECT_TRUE(refused.first);
        EXPECT_TRUE(refused.second);
        ASSERT_EQ(starting.wait_for(20s), std::future_status::ready);
        const auto trusted = starting.get();
        EXPECT_TRUE(trusted) << trusted.error().toString();
    }

    TEST_F(ControlConnectionTlsTests, ImplicitTlsComesBeforeTheGreeting)
    {
        ASSERT_NO_FATAL_FAILURE(restartServer({.implicitTls = true}));
        server_->addConnection(Script{}.reply("220 Ready").expect("NOOP").reply("200 Fine").expectDisconnect());
        const auto connection = makeConnection();
        expectReply(connectWithImplicitTls(connection, settings()), 220);
        ASSERT_TRUE(connection->trustedCertificate());
        EXPECT_EQ(connection->trustedCertificate()->source, TrustSource::Authorities);
        expectReply(execute(connection, command(Verb::Noop)), 200);
    }

    TEST_F(ControlConnectionTlsTests, ImplicitTlsWithAnUntrustedCertificateSendsNothing)
    {
        ASSERT_NO_FATAL_FAILURE(restartServer({.implicitTls = true, .certificate = tlsTestPki().selfSigned}));
        server_->addConnection(Script{}.reply("220 Ready").expectDisconnect());
        const auto connection = makeConnection();
        const auto greeting = connectWithImplicitTls(connection, settings());
        expectError(greeting, RemoteIo::RemoteErrorKind::TrustRequired);
        ASSERT_TRUE(connection->untrustedCertificate());
        EXPECT_EQ(connection->untrustedCertificate()->publicKeySha256, publicKeyDigestOf(tlsTestPki().selfSigned));
        EXPECT_TRUE(commandsAfterDisconnect(connection).empty());
    }

    TEST_F(ControlConnectionTlsTests, QuitOverTlsSendsCloseNotify)
    {
        server_->addConnection(explicitTls().expect("QUIT").reply("221 Goodbye").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        ASSERT_TRUE(startTls(connection, settings()));
        expectReply(execute(connection, command(Verb::Quit)), 221);
        EXPECT_EQ(stateOf(connection), ControlState::Closed);
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_TRUE(hasEntry(server_->transcript(), TranscriptEntry::Kind::Disconnected, "end of stream"));
    }

    TEST_F(ControlConnectionTlsTests, CloseOverTlsSendsCloseNotify)
    {
        server_->addConnection(explicitTls().expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        ASSERT_TRUE(startTls(connection, settings()));
        connection->close();
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_TRUE(hasEntry(server_->transcript(), TranscriptEntry::Kind::Disconnected, "end of stream"));
    }

    TEST_F(ControlConnectionTlsTests, ServerClosingTlsEndsTheConnection)
    {
        server_->addConnection(explicitTls().closeControl());
        const auto connection = makeConnection();
        connect(connection);
        ASSERT_TRUE(startTls(connection, settings()));
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteIo::RemoteErrorKind::ConnectionLost) << ended->toString();
    }

    TEST_F(ControlConnectionTlsTests, RenegotiationRequestEndsTheConnection)
    {
        // The client answers with a no_renegotiation alert, an OpenSSL server ends the connection on it. Nothing is
        // written meanwhile, Asio would keep the alert behind a write in flight until the next one.
        server_->addConnection(explicitTls({.maximumVersion = TLS1_2_VERSION})
                .expect("NOOP")
                .reply("200 Fine")
                .renegotiateTls()
                .expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto trusted = startTls(connection, settings());
        ASSERT_TRUE(trusted) << trusted.error().toString();
        expectReply(execute(connection, command(Verb::Noop)), 200);
        const auto ended = waitForEnd();
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->kind, RemoteIo::RemoteErrorKind::ConnectionLost) << ended->toString();
        EXPECT_FALSE(hasEntry(server_->transcript(), TranscriptEntry::Kind::TlsEstablished, "control renegotiated"));
    }

    TEST_F(ControlConnectionTlsTests, SessionForDataOnceATicketCame)
    {
        server_->addConnection(explicitTls().expect("NOOP").reply("200 Fine").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        ASSERT_TRUE(startTls(connection, settings()));
        // The reply was read after the TLS 1.3 tickets, which come right behind the handshake.
        expectReply(execute(connection, command(Verb::Noop)), 200);
        const auto resumable =
            run(connection,
                [connection]() -> asio::awaitable<bool>
                {
                    co_return connection->sessionForData().has_value();
                });
        EXPECT_TRUE(resumable);
    }

    TEST_F(ControlConnectionTlsTests, NoSessionForDataWithoutTickets)
    {
        server_->addConnection(
            explicitTls({.issueNoTickets = true}).expect("NOOP").reply("200 Fine").expectDisconnect()
        );
        const auto connection = makeConnection();
        connect(connection);
        ASSERT_TRUE(startTls(connection, settings()));
        expectReply(execute(connection, command(Verb::Noop)), 200);
        const auto resumable =
            run(connection,
                [connection]() -> asio::awaitable<bool>
                {
                    co_return connection->sessionForData().has_value();
                });
        EXPECT_FALSE(resumable);
    }

    TEST_F(ControlConnectionTlsTests, NoSessionForDataWithoutTls)
    {
        server_->addConnection(Script{}.reply("220 Ready").expectDisconnect());
        const auto connection = makeConnection();
        connect(connection);
        const auto resumable =
            run(connection,
                [connection]() -> asio::awaitable<bool>
                {
                    co_return connection->sessionForData().has_value();
                });
        EXPECT_FALSE(resumable);
    }

    TEST_F(ControlConnectionTlsTests, DroppingTheLastOwnerDuringATlsReadClosesTheConnection)
    {
        server_->addConnection(explicitTls().expectDisconnect());
        // Not through makeConnection(), which keeps a copy for the end of the test.
        auto connection = ControlConnection::create(pool_.get_executor(), relaxedOptions());
        connect(connection);
        ASSERT_TRUE(startTls(connection, settings()));
        connection.reset();
        EXPECT_TRUE(server_->waitUntilDisconnected());
    }
}
