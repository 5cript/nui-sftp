#pragma once

#include "test_control_connection_tls.hpp"

#include <ftp/client/login.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using LoginOutcome = std::expected<LoginResult, RemoteIo::RemoteError>;

        Credentials testCredentials()
        {
            return Credentials{.user = "user", .password = "password"};
        }

        /**
         * @brief FEAT, OPTS UTF8 ON, TYPE I and PWD answered like vsftpd does.
         */
        Script& afterAuthentication(Script& script)
        {
            return script.featLike(ServerLike::Vsftpd)
                .expect("OPTS", "UTF8 ON")
                .reply("200 Always in UTF8 mode.")
                .expect("TYPE", "I")
                .reply("200 Switching to Binary mode.")
                .expect("PWD")
                .reply("257 \"/home/user\" is the current directory");
        }

        Script& credentials(Script& script)
        {
            return script.expect("USER", "user")
                .reply("331 Password required")
                .expect("PASS", "password")
                .reply("230 Logged in");
        }

        Script& protection(Script& script)
        {
            return script.expect("PBSZ", "0")
                .reply("200 PBSZ set to 0.")
                .expect("PROT", "P")
                .reply("200 PROT now Private.");
        }

        std::vector<std::string> const& plainCommands()
        {
            static const std::vector<std::string> commands{
                "USER user", "PASS password", "FEAT", "OPTS UTF8 ON", "TYPE I", "PWD"
            };
            return commands;
        }
    }

    class LoginTests : public ControlConnectionTlsTests
    {
      protected:
        LoginOutcome login(std::shared_ptr<ControlConnection> const& connection, LoginOptions options)
        {
            return run(
                connection,
                [connection, endpoints = endpoints(), options = std::move(options)]() -> asio::awaitable<LoginOutcome>
                {
                    co_return co_await FileTransferProtocol::logIn(connection, endpoints, options);
                }
            );
        }

        LoginOptions explicitOptions(std::optional<Sha256Digest> pin = std::nullopt) const
        {
            return LoginOptions{.tlsMode = TlsMode::Explicit, .tls = settings(pin), .credentials = testCredentials()};
        }

        void expectLoginError(LoginOutcome const& outcome, RemoteIo::RemoteErrorKind kind)
        {
            ASSERT_FALSE(outcome) << "logged in";
            EXPECT_EQ(outcome.error().kind, kind) << outcome.error().toString();
        }
    };

    TEST_F(LoginTests, PlainLoginRunsTheWholeSequence)
    {
        Script script{};
        script.reply("220 Ready");
        afterAuthentication(credentials(script));
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, LoginOptions{.credentials = testCredentials()});
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(result->greeting.code, 220);
        EXPECT_EQ(result->homeDirectory, "/home/user");
        EXPECT_TRUE(result->features.has("EPSV"));
        EXPECT_TRUE(result->utf8Accepted);
        EXPECT_FALSE(result->dataProtected);
        EXPECT_FALSE(result->certificate);
        EXPECT_EQ(commandsAfterDisconnect(connection), plainCommands());
    }

    TEST_F(LoginTests, ExplicitTlsProtectsTheCredentialsAndTheData)
    {
        auto script = explicitTls();
        protection(credentials(script));
        afterAuthentication(script);
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, explicitOptions());
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_TRUE(result->dataProtected);
        ASSERT_TRUE(result->certificate);
        EXPECT_EQ(result->certificate->source, TrustSource::Authorities);
        EXPECT_EQ(result->homeDirectory, "/home/user");

        connection->close();
        ASSERT_TRUE(server_->waitUntilDisconnected());
        const auto transcript = server_->transcript();
        const auto established = std::ranges::find_if(
            transcript,
            [](TranscriptEntry const& entry)
            {
                return entry.kind == TranscriptEntry::Kind::TlsEstablished;
            }
        );
        const auto user = std::ranges::find_if(
            transcript,
            [](TranscriptEntry const& entry)
            {
                return entry.kind == TranscriptEntry::Kind::Command && entry.detail.starts_with("USER");
            }
        );
        ASSERT_NE(established, transcript.end());
        ASSERT_NE(user, transcript.end());
        EXPECT_LT(established, user);
    }

    TEST_F(LoginTests, ImplicitTlsLogsInOverTlsFromTheStart)
    {
        ASSERT_NO_FATAL_FAILURE(restartServer({.implicitTls = true}));
        Script script{};
        script.reply("220 Ready");
        afterAuthentication(protection(credentials(script)));
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(
            connection, LoginOptions{.tlsMode = TlsMode::Implicit, .tls = settings(), .credentials = testCredentials()}
        );
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_TRUE(result->dataProtected);
        ASSERT_TRUE(result->certificate);
        EXPECT_EQ(commandsAfterDisconnect(connection).front(), "USER user");
    }

    TEST_F(LoginTests, UntrustedCertificateStopsBeforeTheCredentials)
    {
        server_->addConnection(explicitTls({.certificate = tlsTestPki().selfSigned}).expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, explicitOptions());
        expectLoginError(result, RemoteIo::RemoteErrorKind::TrustRequired);
        ASSERT_TRUE(connection->untrustedCertificate());
        EXPECT_EQ(connection->untrustedCertificate()->publicKeySha256, publicKeyDigestOf(tlsTestPki().selfSigned));
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
    }

    TEST_F(LoginTests, PinnedCertificateLogsIn)
    {
        auto script = explicitTls({.certificate = tlsTestPki().selfSigned});
        afterAuthentication(protection(credentials(script)));
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, explicitOptions(publicKeyDigestOf(tlsTestPki().selfSigned)));
        ASSERT_TRUE(result) << result.error().toString();
        ASSERT_TRUE(result->certificate);
        EXPECT_EQ(result->certificate->source, TrustSource::Pin);
    }

    TEST_F(LoginTests, RefusedAuthTlsNeverSendsTheCredentials)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("AUTH", "TLS")
                .reply("530 Please login with USER and PASS.")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        const auto result = login(connection, explicitOptions());
        expectLoginError(result, RemoteIo::RemoteErrorKind::NoSecureConnection);
        EXPECT_EQ(commandsAfterDisconnect(connection), (std::vector<std::string>{"AUTH TLS"}));
        EXPECT_EQ(stateOf(connection), ControlState::Dead);
    }

    TEST_F(LoginTests, UserAloneMayLogIn)
    {
        Script script{};
        script.reply("220 Ready").expect("USER", "user").reply("230 Logged in without a password");
        server_->addConnection(afterAuthentication(script).expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, LoginOptions{.credentials = testCredentials()});
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(commandsAfterDisconnect(connection).at(1u), "FEAT");
    }

    TEST_F(LoginTests, PasswordAnsweredWith202IsALogin)
    {
        // 202: the command was superfluous, the login needs no password after all (RFC 959 5.4).
        Script script{};
        script.reply("220 Ready")
            .expect("USER", "user")
            .reply("331 Password required")
            .expect("PASS", "password")
            .reply("202 Already logged in");
        server_->addConnection(afterAuthentication(script).expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, LoginOptions{.credentials = testCredentials()});
        ASSERT_TRUE(result) << result.error().toString();
    }

    TEST_F(LoginTests, AccountIsSentWhenAskedFor)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("USER", "user")
            .reply("331 Password required")
            .expect("PASS", "password")
            .reply("332 Need account for login")
            .expect("ACCT", "billing")
            .reply("230 Logged in");
        server_->addConnection(afterAuthentication(script).expectDisconnect());
        const auto connection = makeConnection();
        auto credentials = testCredentials();
        credentials.account = "billing";
        const auto result = login(connection, LoginOptions{.credentials = credentials});
        ASSERT_TRUE(result) << result.error().toString();
    }

    TEST_F(LoginTests, AccountAskedForButNoneSetFails)
    {
        server_->addConnection(
            Script{}
                .reply("220 Ready")
                .expect("USER", "user")
                .reply("331 Password required")
                .expect("PASS", "password")
                .reply("332 Need account for login")
                .expectDisconnect()
        );
        const auto connection = makeConnection();
        const auto result = login(connection, LoginOptions{.credentials = testCredentials()});
        expectLoginError(result, RemoteIo::RemoteErrorKind::AuthenticationFailed);
        EXPECT_EQ(stateOf(connection), ControlState::Closed);
    }

    struct RefusedLoginCase
    {
        std::string_view name{};
        Script script{};
        RemoteIo::RemoteErrorKind kind{};
    };

    void PrintTo(RefusedLoginCase const& refused, std::ostream* stream)
    {
        *stream << refused.name;
    }

    class LoginRefusedTests
        : public LoginTests
        , public ::testing::WithParamInterface<RefusedLoginCase>
    {};

    TEST_P(LoginRefusedTests, ServerRefusalIsMapped)
    {
        auto script = GetParam().script;
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, LoginOptions{.credentials = testCredentials()});
        expectLoginError(result, GetParam().kind);
        EXPECT_NE(stateOf(connection), ControlState::Open);
    }

    namespace
    {
        Script refusedAfter(std::string_view verb, std::string_view reply)
        {
            Script script{};
            script.reply("220 Ready");
            credentials(script);
            if (verb == "FEAT")
                return std::move(script.expect("FEAT").reply(reply));
            script.featLike(ServerLike::Vsftpd).expect("OPTS", "UTF8 ON").reply("200 Fine");
            if (verb == "TYPE")
                return std::move(script.expect("TYPE", "I").reply(reply));
            return std::move(script.expect("TYPE", "I").reply("200 Fine").expect("PWD").reply(reply));
        }

        std::vector<RefusedLoginCase> refusedLogins()
        {
            Script wrongPassword{};
            wrongPassword.reply("220 Ready")
                .expect("USER", "user")
                .reply("331 Password required")
                .expect("PASS", "password")
                .reply("530 Login incorrect.");
            Script proftpdLimit{};
            proftpdLimit.reply("220 Ready")
                .expect("USER", "user")
                .reply("331 Password required")
                .expect("PASS", "password")
                .reply("530 Sorry, the maximum number of clients (3) from your host are already connected.");
            Script unknownUser{};
            unknownUser.reply("220 Ready").expect("USER", "user").reply("530 This FTP server is anonymous only.");
            Script vsftpdLimit{};
            vsftpdLimit.reply("421 There are too many connections from your internet address.");
            return {
                {.name = "WrongPassword",
                    .script = wrongPassword,
                    .kind = RemoteIo::RemoteErrorKind::AuthenticationFailed},
                {.name = "ConnectionLimitAfterPass",
                    .script = proftpdLimit,
                    .kind = RemoteIo::RemoteErrorKind::ConnectionLimit},
                {.name = "UserRefused", .script = unknownUser, .kind = RemoteIo::RemoteErrorKind::AuthenticationFailed},
                {.name = "ConnectionLimitAtTheGreeting",
                    .script = vsftpdLimit,
                    .kind = RemoteIo::RemoteErrorKind::ConnectionLimit},
                {.name = "FeatFailed",
                    .script = refusedAfter("FEAT", "451 Local error"),
                    .kind = RemoteIo::RemoteErrorKind::Failure},
                {.name = "BinaryTypeRefused",
                    .script = refusedAfter("TYPE", "504 Type not implemented"),
                    .kind = RemoteIo::RemoteErrorKind::Unsupported},
                {.name = "PwdWithoutAPath",
                    .script = refusedAfter("PWD", "257 is the current directory"),
                    .kind = RemoteIo::RemoteErrorKind::Failure},
            };
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Replies,
        LoginRefusedTests,
        ::testing::ValuesIn(refusedLogins()),
        [](::testing::TestParamInfo<RefusedLoginCase> const& info)
        {
            return std::string{info.param.name};
        }
    );

    TEST_F(LoginTests, RefusedDataProtectionFailsTheLogin)
    {
        auto script = explicitTls();
        credentials(script)
            .expect("PBSZ", "0")
            .reply("200 PBSZ set to 0.")
            .expect("PROT", "P")
            .reply("536 Level not supported");
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, explicitOptions());
        expectLoginError(result, RemoteIo::RemoteErrorKind::NoSecureConnection);
        EXPECT_EQ(result.error().protocolCode, 536);
        EXPECT_EQ(stateOf(connection), ControlState::Closed);
    }

    TEST_F(LoginTests, ServerWithoutFeatLogsInWithoutFeatures)
    {
        Script script{};
        script.reply("220 Ready");
        credentials(script)
            .expect("FEAT")
            .reply("502 Command not implemented")
            .expect("OPTS", "UTF8 ON")
            .reply("501 Option not understood")
            .expect("TYPE", "I")
            .reply("200 Fine")
            .expect("PWD")
            .reply("257 \"/\"");
        server_->addConnection(script.expectDisconnect());
        const auto connection = makeConnection();
        const auto result = login(connection, LoginOptions{.credentials = testCredentials()});
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_TRUE(result->features.all().empty());
        EXPECT_FALSE(result->utf8Accepted);
        EXPECT_EQ(result->homeDirectory, "/");
    }

    TEST_F(LoginTests, TlsWithoutItsSettingsSendsNothing)
    {
        const auto connection = makeConnection();
        const auto result =
            login(connection, LoginOptions{.tlsMode = TlsMode::Explicit, .credentials = testCredentials()});
        expectLoginError(result, RemoteIo::RemoteErrorKind::Failure);
        EXPECT_TRUE(server_->transcript().empty());
    }
}
