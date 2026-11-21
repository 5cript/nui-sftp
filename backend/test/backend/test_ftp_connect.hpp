#pragma once

#include <backend/ftp/ftp_connect.hpp>
#include <backend/ftp/ftp_engine_context.hpp>
#include <backend/password/password_provider.hpp>
#include <backend/session_manager.hpp>
#include <ftp/support/scripted_ftp_server.hpp>
#include <ftp/support/test_pki.hpp>
#include <ftp/support/tree_responder.hpp>
#include <ids/ids.hpp>
#include <paths/native.hpp>
#include <persistence/state/ftp_options.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state_holder.hpp>
#include <remote_io/remote_file_system.hpp>
#include <utility/temporary_directory.hpp>
#include "nui_env.hpp"
#include "test_environment.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/thread_pool.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace FtpConnectTesting
    {
        using namespace std::chrono_literals;
        using FileTransferProtocol::FtpConnectionOptions;
        using FileTransferProtocol::Test::Responder;
        using FileTransferProtocol::Test::Script;
        using FileTransferProtocol::Test::ScriptedFtpServer;
        using FileTransferProtocol::Test::ServedTree;
        using FileTransferProtocol::Test::TreeResponder;
        using FileTransferProtocol::Test::servedDirectory;
        using FileTransferProtocol::Test::servedFile;

        Persistence::FtpSessionOptions sessionFor(std::string host, std::optional<std::string> user = "alice")
        {
            Persistence::FtpSessionOptions session{};
            session.host = std::move(host);
            if (user)
                session.users = {Persistence::FtpUser{.name = std::move(user), .account = std::nullopt}};
            return session;
        }

        std::shared_ptr<FileTransferProtocol::TlsContext> testTlsContext()
        {
            FileTransferProtocol::TlsContextOptions options{};
            options.systemAuthorities = false;
            auto context = FileTransferProtocol::TlsContext::create(std::move(options));
            if (!context)
                throw std::runtime_error{context.error().toString()};
            return std::move(context).value();
        }

        /**
         * @brief Serves a tree, but answers a line that @p rule picks as the rule says.
         */
        class TroubledTreeResponder : public Responder
        {
          public:
            using Rule = std::function<std::optional<Script>(std::string_view line)>;

            TroubledTreeResponder(std::shared_ptr<ServedTree> tree, Rule rule)
                : tree_{std::move(tree)}
                , rule_{std::move(rule)}
            {}

            Script answer(std::string_view line) override
            {
                if (auto special = rule_(line))
                    return std::move(*special);
                return tree_.answer(line);
            }

          private:
            TreeResponder tree_;
            Rule rule_;
        };

        std::optional<Script> noTrouble(std::string_view)
        {
            return std::nullopt;
        }

        /**
         * @brief A listing of /slow never ends.
         */
        std::optional<Script> stallsTheSlowListing(std::string_view line)
        {
            if (line != "CWD /slow")
                return std::nullopt;
            Script script{};
            script.checkpoint("listing stalled").stall();
            return script;
        }

        /**
         * @brief The control connection breaks off at the first listing.
         */
        std::optional<Script> resetsAtTheFirstListing(std::string_view line)
        {
            if (!line.starts_with("CWD"))
                return std::nullopt;
            Script script{};
            script.resetControl();
            return script;
        }

        /**
         * @brief FEAT lists neither MLST nor SIZE, so nothing can check an upload destination.
         */
        std::optional<Script> listsNoSize(std::string_view line)
        {
            if (line != "FEAT")
                return std::nullopt;
            Script script{};
            script.reply("211-Features:\n EPSV\n MDTM\n PASV\n REST STREAM\n UTF8\n211 End");
            return script;
        }

        /**
         * @brief The greeting and a login that takes @p user and @p password, then the tree as @p rule allows.
         */
        Script loggingIn(
            std::shared_ptr<ServedTree> tree,
            std::string const& user,
            std::string const& password,
            TroubledTreeResponder::Rule rule = noTrouble
        )
        {
            Script script{};
            script.reply("220 Ready")
                .expect("USER", user)
                .reply("331 Password required")
                .expect("PASS", password)
                .reply("230 Logged in")
                .serve(std::make_shared<TroubledTreeResponder>(std::move(tree), std::move(rule)));
            return script;
        }

        /**
         * @brief The greeting, then USER is never answered: a login to a server that went silent.
         */
        Script silentAfterTheGreeting()
        {
            Script script{};
            script.reply("220 Ready").expect("USER").checkpoint("login stalled").stall();
            return script;
        }

        /**
         * @brief Accepts and never greets: a server that went silent before a reach got anywhere.
         */
        Script silentBeforeTheGreeting()
        {
            Script script{};
            script.checkpoint("reach stalled").stall();
            return script;
        }

        /**
         * @brief A spare connection for one the pool may log in while the session closes, any login is taken.
         */
        Script spare(std::shared_ptr<ServedTree> tree)
        {
            Script script{};
            script.reply("220 Ready").serve(std::make_shared<TroubledTreeResponder>(std::move(tree), noTrouble));
            return script;
        }

        Script refusingTheLogin(std::string const& user, std::string const& password)
        {
            Script script{};
            script.reply("220 Ready")
                .expect("USER", user)
                .reply("331 Password required")
                .expect("PASS", password)
                .reply("530 Login incorrect.")
                .expectDisconnect();
            return script;
        }

        /**
         * @brief The connection that reaches the server before the password is asked for: the greeting and QUIT.
         */
        Script reaching()
        {
            Script script{};
            script.reply("220 Ready").expect("QUIT").reply("221 Goodbye").expectDisconnect();
            return script;
        }

        FtpConnect::PasswordSource answering(std::string password)
        {
            return [password = std::move(password)](std::string const&) -> std::optional<std::string>
            {
                return password;
            };
        }

        FtpConnect::PasswordSource neverAsked()
        {
            return [](std::string const& prompt) -> std::optional<std::string>
            {
                ADD_FAILURE() << "asked for a password: " << prompt;
                return std::nullopt;
            };
        }

        FtpConnect::TrustSource neverAskedAboutTrust()
        {
            return [](FtpConnect::TrustQuestion const& question)
            {
                ADD_FAILURE() << "asked whether to trust the certificate of " << question.host;
                return FtpConnect::TrustAnswer::Refused;
            };
        }

        /**
         * @brief What the prompts of one connect were asked, in their order: "trust" and "password".
         */
        struct Prompts
        {
            std::vector<std::string> order{};
            std::vector<FtpConnect::TrustQuestion> trustQuestions{};
        };

        /**
         * @brief Records the questions in @p prompts and gives @p answer to each.
         */
        FtpConnect::TrustSource answeringTrust(std::shared_ptr<Prompts> prompts, FtpConnect::TrustAnswer answer)
        {
            return [prompts = std::move(prompts), answer](FtpConnect::TrustQuestion const& question)
            {
                prompts->order.emplace_back("trust");
                prompts->trustQuestions.push_back(question);
                return answer;
            };
        }

        /**
         * @brief Records the password prompt in @p prompts and declines it, so the connect ends before its login.
         */
        FtpConnect::PasswordSource decliningThePassword(std::shared_ptr<Prompts> prompts)
        {
            return [prompts = std::move(prompts)](std::string const&) -> std::optional<std::string>
            {
                prompts->order.emplace_back("password");
                return std::nullopt;
            };
        }

        /**
         * @brief Has the TLS contexts made while it lives trust the authority of the tests, as an authority of the
         *        system: OpenSSL reads SSL_CERT_FILE for its default authorities.
         */
        class AuthorityOfTheTests
        {
          public:
            explicit AuthorityOfTheTests(std::string const& authorityPem)
            {
                std::ofstream writer{file(), std::ios_base::binary};
                writer << authorityPem;
            }

          private:
            std::filesystem::path file() const
            {
                return directory_.path() / "authority.pem";
            }

            Utility::TemporaryDirectory directory_{temporaryRoot, true};
            ScopedVariable certificateFile_{"SSL_CERT_FILE", Paths::nativeToUtf8(file())};
        };

        /**
         * @brief The greeting and AUTH TLS, answered with @p certificate.
         */
        Script& explicitTlsWith(Script& script, FileTransferProtocol::Test::CertifiedKey const& certificate)
        {
            FileTransferProtocol::Test::TlsBehaviour behaviour{};
            behaviour.certificate = certificate;
            return script.reply("220 Ready").expect("AUTH", "TLS").reply("234 Proceed").startTls(behaviour);
        }

        /**
         * @brief A reach over explicit TLS whose @p certificate the client refuses.
         */
        Script refusedOverTls(FileTransferProtocol::Test::CertifiedKey const& certificate)
        {
            Script script{};
            explicitTlsWith(script, certificate).expectDisconnect();
            return script;
        }

        /**
         * @brief A reach over explicit TLS whose @p certificate the client takes: the handshake and QUIT.
         */
        Script reachingOverTls(FileTransferProtocol::Test::CertifiedKey const& certificate)
        {
            Script script{};
            explicitTlsWith(script, certificate).expect("QUIT").reply("221 Goodbye").expectDisconnect();
            return script;
        }

        Paths::RemotePath remotePath(std::string_view path)
        {
            return Paths::RemotePath::make(path).value();
        }

        /**
         * @brief Counts the losses a connection reports and tells of the first.
         */
        struct CountedLosses
        {
            void record()
            {
                if (count.fetch_add(1) == 0)
                    first.set_value();
            }

            std::atomic<int> count{0};
            std::promise<void> first{};
        };

        /**
         * @brief Answers every prompt with its answer and keeps what it was asked.
         */
        class AnsweringProvider : public PasswordProvider
        {
          public:
            struct Question
            {
                std::string whatFor{};
                std::string prompt{};
            };

            explicit AnsweringProvider(std::optional<std::string> answer)
                : answer_{std::move(answer)}
            {}

            void getPassword(
                std::string const& whatFor,
                std::string const& prompt,
                std::function<void(std::optional<std::string>)> const& onPasswordReady
            ) override
            {
                {
                    std::scoped_lock lock{mutex_};
                    questions_.push_back({.whatFor = whatFor, .prompt = prompt});
                }
                onPasswordReady(answer_);
            }

            std::vector<Question> questions() const
            {
                std::scoped_lock lock{mutex_};
                return questions_;
            }

          private:
            const std::optional<std::string> answer_;
            mutable std::mutex mutex_{};
            std::vector<Question> questions_{};
        };

        /**
         * @brief Answers trust prompts as it was told, and keeps what it was asked.
         */
        class AnsweringTrustProvider : public FtpConnect::TrustProvider
        {
          public:
            enum class Answer
            {
                Trust,
                Refuse,
                /** @brief Leaves the prompt waiting. */
                Never,
            };

            explicit AnsweringTrustProvider(Answer answer)
                : answer_{answer}
            {}

            void askTrust(
                FtpConnect::TrustQuestion const& question,
                std::function<void(FtpConnect::TrustAnswer)> onAnswer
            ) override
            {
                {
                    std::scoped_lock lock{mutex_};
                    questions_.push_back(question);
                    if (questions_.size() == 1u)
                        firstAsked_.set_value();
                }
                switch (answer_)
                {
                    case Answer::Trust:
                        return onAnswer(FtpConnect::TrustAnswer::Once);
                    case Answer::Refuse:
                        return onAnswer(FtpConnect::TrustAnswer::Refused);
                    case Answer::Never:
                        return;
                }
            }

            std::vector<FtpConnect::TrustQuestion> questions() const
            {
                std::scoped_lock lock{mutex_};
                return questions_;
            }

            /**
             * @brief Ready once the first prompt was asked. Call once.
             */
            std::future<void> firstAsked()
            {
                return firstAsked_.get_future();
            }

          private:
            const Answer answer_;
            mutable std::mutex mutex_{};
            std::vector<FtpConnect::TrustQuestion> questions_{};
            std::promise<void> firstAsked_{};
        };
    }

    TEST(FtpTrustQuestionTests, TheFrontendGetsTheCertificateWithUtcTimesAndFingerprints)
    {
        using namespace std::chrono;
        FileTransferProtocol::Sha256Digest publicKey{};
        publicKey.fill(0x01u);
        FileTransferProtocol::Sha256Digest certificate{};
        certificate.fill(0xABu);
        const FtpConnect::TrustQuestion question{
            .host = "ftp.example.com",
            .port = 990u,
            .certificate =
                {
                    .subject = "CN=ftp.example.com",
                    .issuer = "CN=Example Authority",
                    .notBefore = sys_seconds{sys_days{year{2026} / 1 / 2}} + hours{3} + minutes{4} + seconds{5},
                    .notAfter = sys_seconds{sys_days{year{2027} / 12 / 31}},
                    .subjectAlternativeNames = {"DNS:ftp.example.com", "IP:192.0.2.1"},
                    .certificateSha256 = certificate,
                    .publicKeySha256 = publicKey,
                    .verifyFailures =
                        {{.kind = FileTransferProtocol::VerifyFailureKind::SelfSigned,
                            .reason = "self-signed certificate"}},
                },
            .pinnedKeyDiffers = true,
        };

        const auto json = FtpConnect::toJson(question);
        EXPECT_EQ(json.at("host"), "ftp.example.com");
        EXPECT_EQ(json.at("port"), 990);
        EXPECT_EQ(json.at("subject"), "CN=ftp.example.com");
        EXPECT_EQ(json.at("issuer"), "CN=Example Authority");
        EXPECT_EQ(json.at("notBefore"), "2026-01-02 03:04:05 UTC");
        EXPECT_EQ(json.at("notAfter"), "2027-12-31 00:00:00 UTC");
        EXPECT_EQ(json.at("subjectAlternativeNames"), nlohmann::json::array({"DNS:ftp.example.com", "IP:192.0.2.1"}));
        EXPECT_EQ(
            FileTransferProtocol::parseDigest(json.at("publicKeySha256").get<std::string>()), std::optional{publicKey}
        );
        EXPECT_EQ(
            FileTransferProtocol::parseDigest(json.at("certificateSha256").get<std::string>()),
            std::optional{certificate}
        );
        EXPECT_EQ(
            json.at("verifyFailures"),
            nlohmann::json::array({{{"kind", "selfSigned"}, {"reason", "self-signed certificate"}}})
        );
        EXPECT_EQ(json.at("pinnedKeyDiffers"), true);
    }

    TEST(FtpConnectOptionsTests, AnonymousLoginsNeedNoPassword)
    {
        using Persistence::FtpUser;
        for (auto const& name : std::vector<std::optional<std::string>>{std::nullopt, "", "anonymous", "FTP", "Ftp"})
            EXPECT_TRUE(FtpConnect::isAnonymous(FtpUser{.name = name, .account = std::nullopt}))
                << name.value_or("none");
        EXPECT_FALSE(FtpConnect::isAnonymous(FtpUser{.name = "alice", .account = std::nullopt}));
        EXPECT_FALSE(FtpConnect::isAnonymous(FtpUser{.name = "ftpuser", .account = std::nullopt}));

        const auto nameless = FtpConnect::credentialsOf(FtpUser{});
        EXPECT_EQ(nameless.user, "anonymous");
        EXPECT_EQ(nameless.password, "anonymous@");
        const auto named = FtpConnect::credentialsOf(FtpUser{.name = "ftp", .account = std::nullopt});
        EXPECT_EQ(named.user, "ftp");
        EXPECT_EQ(named.password, "anonymous@");
        const auto alice = FtpConnect::credentialsOf(FtpUser{.name = "alice", .account = "billing"});
        EXPECT_EQ(alice.user, "alice");
        EXPECT_TRUE(alice.password.empty());
        EXPECT_EQ(alice.account, "billing");
        // A cleared settings field.
        EXPECT_FALSE(FtpConnect::credentialsOf(FtpUser{.name = "alice", .account = ""}).account);
    }

    TEST(FtpConnectOptionsTests, ThePromptNamesTheUserAndTheServer)
    {
        EXPECT_EQ(FtpConnect::passwordPrompt("alice", "ftp.example.com"), "Password for alice@ftp.example.com:");
    }

    TEST(FtpConnectOptionsTests, SavedOptionsBecomeTheOptionsOfTheClient)
    {
        using namespace FtpConnectTesting;
        auto session = sessionFor("ftp.example.com");
        session.port = 2121;
        session.tlsMode = Persistence::FtpTlsMode::Plain;
        session.ftpOptions.value() = Persistence::FtpOptions{
            .connectTimeoutSeconds = 7,
            .commandTimeoutSeconds = 8,
            .dataInactivityTimeoutSeconds = 9,
            .transferReplyTimeoutSeconds = 10,
            .maxConnections = 3,
            .extendedPassive = Persistence::FtpExtendedPassive::Never,
            .trustPassiveAddress = true,
            .keepIdleAlive = true,
            .idleNoopIntervalSeconds = 40,
            .keepaliveIdleSeconds = 11,
            .keepaliveIntervalSeconds = 4,
            .keepaliveCount = 6,
            .minimumTlsVersion = std::nullopt,
        };
        session.sftpOptions.value().concurrency = 5;

        const auto mapped = FtpConnect::connectionOptionsOf(session, nullptr);
        ASSERT_TRUE(mapped) << mapped.error();
        auto const& pool = mapped->pool;
        auto const& transfer = mapped->fileSystem.transfer;
        EXPECT_EQ(pool.host, "ftp.example.com");
        EXPECT_EQ(pool.port, 2121u);
        EXPECT_EQ(pool.login.tlsMode, FileTransferProtocol::TlsMode::None);
        EXPECT_FALSE(pool.login.tls);
        EXPECT_EQ(pool.login.credentials.user, "alice");
        EXPECT_TRUE(pool.login.credentials.password.empty());
        EXPECT_EQ(pool.control.connectTimeout, 7s);
        EXPECT_EQ(transfer.data.connectTimeout, 7s);
        EXPECT_EQ(pool.control.commandTimeout, 8s);
        EXPECT_EQ(transfer.data.inactivityTimeout, 9s);
        EXPECT_EQ(transfer.finalReplyTimeout, 10s);
        EXPECT_EQ(pool.transferConnections, 2u);
        EXPECT_TRUE(transfer.data.extendedPassive == FileTransferProtocol::ExtendedPassive::Never);
        EXPECT_TRUE(transfer.data.trustPassiveAddress);
        EXPECT_TRUE(pool.keepIdleAlive);
        EXPECT_EQ(pool.idleNoopInterval, 40s);
        EXPECT_EQ(pool.control.socket.keepalive.idle, 11s);
        EXPECT_EQ(pool.control.socket.keepalive.interval, 4s);
        EXPECT_EQ(pool.control.socket.keepalive.count, 6);
        // No timeout is above its default, the backstops stay.
        const FtpConnectionOptions defaults{};
        EXPECT_EQ(mapped->fileSystem.operationLimits.backstop, defaults.fileSystem.operationLimits.backstop);
        EXPECT_EQ(mapped->fileSystem.transferCallLimits.backstop, defaults.fileSystem.transferCallLimits.backstop);
        EXPECT_FALSE(mapped->loginLimits);
    }

    TEST(FtpConnectOptionsTests, UnsetOptionsKeepTheDefaultsOfTheClient)
    {
        using namespace FtpConnectTesting;
        const auto session = sessionFor("ftp.example.com");
        const auto context = testTlsContext();

        const auto mapped = FtpConnect::connectionOptionsOf(session, context);
        ASSERT_TRUE(mapped) << mapped.error();
        const FtpConnectionOptions defaults{};
        auto const& pool = mapped->pool;
        auto const& transfer = mapped->fileSystem.transfer;
        EXPECT_EQ(pool.port, 21u);
        EXPECT_EQ(pool.login.tlsMode, FileTransferProtocol::TlsMode::Explicit);
        ASSERT_TRUE(pool.login.tls);
        EXPECT_EQ(pool.login.tls->context, context);
        EXPECT_EQ(pool.login.tls->serverName, "ftp.example.com");
        EXPECT_FALSE(pool.login.tls->pinnedPublicKey);
        EXPECT_EQ(pool.control.connectTimeout, defaults.pool.control.connectTimeout);
        EXPECT_EQ(pool.control.commandTimeout, defaults.pool.control.commandTimeout);
        EXPECT_EQ(transfer.data.connectTimeout, defaults.fileSystem.transfer.data.connectTimeout);
        EXPECT_EQ(transfer.data.inactivityTimeout, defaults.fileSystem.transfer.data.inactivityTimeout);
        EXPECT_EQ(transfer.finalReplyTimeout, defaults.fileSystem.transfer.finalReplyTimeout);
        EXPECT_TRUE(transfer.data.extendedPassive == defaults.fileSystem.transfer.data.extendedPassive);
        EXPECT_EQ(transfer.data.trustPassiveAddress, defaults.fileSystem.transfer.data.trustPassiveAddress);
        EXPECT_EQ(pool.keepIdleAlive, defaults.pool.keepIdleAlive);
        EXPECT_EQ(pool.idleNoopInterval, defaults.pool.idleNoopInterval);
        EXPECT_EQ(pool.control.socket.keepalive.idle, defaults.pool.control.socket.keepalive.idle);
        EXPECT_EQ(pool.control.socket.keepalive.interval, defaults.pool.control.socket.keepalive.interval);
        EXPECT_EQ(pool.control.socket.keepalive.count, defaults.pool.control.socket.keepalive.count);
        // The default concurrency of 4 transfers, below the default of 4 connections with C0.
        EXPECT_EQ(pool.transferConnections, defaults.pool.transferConnections);
        EXPECT_EQ(mapped->fileSystem.operationLimits.backstop, defaults.fileSystem.operationLimits.backstop);
        EXPECT_EQ(mapped->fileSystem.transferCallLimits.backstop, defaults.fileSystem.transferCallLimits.backstop);
    }

    TEST(FtpConnectOptionsTests, AnImplicitSessionUsesPort990AndPinsNoKeyOfItsOwn)
    {
        using namespace FtpConnectTesting;
        auto session = sessionFor("ftp.example.com");
        session.tlsMode = Persistence::FtpTlsMode::Implicit;

        const auto mapped = FtpConnect::connectionOptionsOf(session, testTlsContext());
        ASSERT_TRUE(mapped) << mapped.error();
        EXPECT_EQ(mapped->pool.port, 990u);
        EXPECT_EQ(mapped->pool.login.tlsMode, FileTransferProtocol::TlsMode::Implicit);
        ASSERT_TRUE(mapped->pool.login.tls);
        // The pin comes from the trust store of the backend at the connect, a session names none.
        EXPECT_FALSE(mapped->pool.login.tls->pinnedPublicKey);
    }

    TEST(FtpConnectOptionsTests, TheTransferConnectionsAreTheConcurrencyBelowTheConnectionLimit)
    {
        using namespace FtpConnectTesting;
        struct Case
        {
            std::optional<int> concurrency{};
            std::optional<int> maxConnections{};
            std::size_t transferConnections{};
        };
        for (
            auto const& [concurrency, maxConnections, transferConnections] : std::vector<Case>{
                {.concurrency = 1, .maxConnections = 8, .transferConnections = 1u},
                {.concurrency = 8, .maxConnections = 3, .transferConnections = 2u},
                {.concurrency = 2, .maxConnections = 2, .transferConnections = 1u},
                {.concurrency = 8, .maxConnections = std::nullopt, .transferConnections = 3u},
                {.concurrency = std::nullopt, .maxConnections = 8, .transferConnections = 4u},
            }
        )
        {
            auto session = sessionFor("ftp.example.com");
            session.tlsMode = Persistence::FtpTlsMode::Plain;
            session.sftpOptions.value().concurrency = concurrency;
            session.ftpOptions.value().maxConnections = maxConnections;
            const auto mapped = FtpConnect::connectionOptionsOf(session, nullptr);
            ASSERT_TRUE(mapped) << mapped.error();
            EXPECT_EQ(mapped->pool.transferConnections, transferConnections)
                << concurrency.value_or(-1) << " transfers, " << maxConnections.value_or(-1) << " connections";
        }
    }

    TEST(FtpConnectOptionsTests, AnOptionOutOfRangeFailsWithItsName)
    {
        using namespace FtpConnectTesting;
        using Change = std::function<void(Persistence::FtpSessionOptions&)>;
        const std::vector<std::pair<Change, std::string>> cases{
            {[](auto& session)
                {
                    session.ftpOptions.value().maxConnections = 1;
                },
                "maxConnections"},
            {[](auto& session)
                {
                    session.ftpOptions.value().connectTimeoutSeconds = 0;
                },
                "connectTimeoutSeconds"},
            {[](auto& session)
                {
                    session.ftpOptions.value().idleNoopIntervalSeconds = -5;
                },
                "idleNoopIntervalSeconds"},
            {[](auto& session)
                {
                    session.ftpOptions.value().keepaliveCount = 0;
                },
                "keepaliveCount"},
            {[](auto& session)
                {
                    session.sftpOptions.value().concurrency = 0;
                },
                "concurrency"},
            {[](auto& session)
                {
                    session.port = 70000;
                },
                "70000"},
            {[](auto& session)
                {
                    session.host.clear();
                },
                "no server"},
        };
        for (auto const& [change, named] : cases)
        {
            auto session = sessionFor("ftp.example.com");
            session.tlsMode = Persistence::FtpTlsMode::Plain;
            change(session);
            const auto mapped = FtpConnect::connectionOptionsOf(session, testTlsContext());
            ASSERT_FALSE(mapped) << "accepted what should name " << named;
            EXPECT_NE(mapped.error().find(named), std::string::npos) << mapped.error();
        }
    }

    TEST(FtpConnectOptionsTests, RaisedTimeoutsMoveTheBackstopsBehindThem)
    {
        using namespace FtpConnectTesting;
        using std::chrono::seconds;
        struct Case
        {
            Persistence::FtpOptions raised{};
            seconds operationBackstop{};
            seconds transferCallBackstop{};
        };
        const FtpConnectionOptions defaults{};
        const auto operationDefault = defaults.fileSystem.operationLimits.backstop;
        const auto transferCallDefault = defaults.fileSystem.transferCallLimits.backstop;
        std::vector<Case> cases(3u);
        // The end of a transfer waits for its data, then for the final reply: 30 s and 45 s over their defaults.
        cases[0].raised.dataInactivityTimeoutSeconds = 60;
        cases[0].raised.transferReplyTimeoutSeconds = 60;
        cases[0].operationBackstop = 75s;
        cases[0].transferCallBackstop = 75s;
        // 45 s more for each command a listing and the call that opens a transfer wait for.
        cases[1].raised.commandTimeoutSeconds = 60;
        cases[1].operationBackstop = 180s;
        cases[1].transferCallBackstop = 225s;
        // 5 s more for each connect: the wait for C0 of an operation and the data connection of both.
        cases[2].raised.connectTimeoutSeconds = 20;
        cases[2].operationBackstop = 10s;
        cases[2].transferCallBackstop = 5s;
        for (auto const& [raised, operationBackstop, transferCallBackstop] : cases)
        {
            auto session = sessionFor("ftp.example.com");
            session.tlsMode = Persistence::FtpTlsMode::Plain;
            session.ftpOptions.value() = raised;
            const auto mapped = FtpConnect::connectionOptionsOf(session, nullptr);
            ASSERT_TRUE(mapped) << mapped.error();
            EXPECT_EQ(mapped->fileSystem.operationLimits.backstop, operationDefault + operationBackstop);
            EXPECT_EQ(mapped->fileSystem.transferCallLimits.backstop, transferCallDefault + transferCallBackstop);
        }

        // The steps of the examples run within their backstops at their full timeouts.
        auto session = sessionFor("ftp.example.com");
        session.tlsMode = Persistence::FtpTlsMode::Plain;
        session.ftpOptions.value() = cases[0].raised;
        const auto finish = FtpConnect::connectionOptionsOf(session, nullptr);
        ASSERT_TRUE(finish) << finish.error();
        EXPECT_GE(finish->fileSystem.transferCallLimits.backstop, 120s) << "the data and the final reply take 120 s";
        session.ftpOptions.value() = cases[1].raised;
        const auto rename = FtpConnect::connectionOptionsOf(session, nullptr);
        ASSERT_TRUE(rename) << rename.error();
        EXPECT_GE(rename->fileSystem.operationLimits.backstop, 120s) << "RNFR and RNTO take 120 s";
    }

    TEST(FtpConnectOptionsTests, TheTlsContextTrustsTheSystemFromTheMinimumVersion)
    {
        const auto unset = FtpConnect::tlsContextOptionsOf(Persistence::FtpOptions{});
        EXPECT_TRUE(unset.systemAuthorities);
        EXPECT_TRUE(unset.minimumVersion == FileTransferProtocol::TlsContextOptions{}.minimumVersion);
        Persistence::FtpOptions options{};
        options.minimumTlsVersion = Persistence::FtpTlsVersion::Tls13;
        EXPECT_TRUE(FtpConnect::tlsContextOptionsOf(options).minimumVersion == FileTransferProtocol::TlsVersion::Tls13);
    }

    TEST(FtpConnectOptionsTests, LoginFailuresSayWhatWentWrong)
    {
        using namespace FtpConnectTesting;
        using RemoteIo::RemoteErrorKind;
        auto session = sessionFor("ftp.example.com");
        const auto failure = [](RemoteErrorKind kind, std::string message)
        {
            FileTransferProtocol::LoginFailure loginFailure{};
            loginFailure.error.kind = kind;
            loginFailure.error.protocol = RemoteIo::Protocol::Ftp;
            loginFailure.error.message = std::move(message);
            return loginFailure;
        };

        EXPECT_EQ(
            FtpConnect::loginFailureMessage(
                failure(RemoteErrorKind::AuthenticationFailed, "Login incorrect."), session
            ),
            "The FTP server ftp.example.com:21 refused the login of alice: Login incorrect."
        );
        EXPECT_EQ(
            FtpConnect::loginFailureMessage(failure(RemoteErrorKind::NoConnection, ""), session),
            "The FTP server ftp.example.com:21 could not be reached."
        );
        EXPECT_NE(
            FtpConnect::loginFailureMessage(failure(RemoteErrorKind::NoSecureConnection, "AUTH refused"), session)
                .find("TLS could not be set up"),
            std::string::npos
        );
        session.tlsMode = Persistence::FtpTlsMode::Plain;
        EXPECT_NE(
            FtpConnect::loginFailureMessage(
                failure(RemoteErrorKind::NoSecureConnection, "Non-anonymous sessions must use encryption."), session
            )
                .find("requires TLS"),
            std::string::npos
        );

        auto untrusted = failure(RemoteErrorKind::TrustRequired, "");
        FileTransferProtocol::CertificateInfo certificate{};
        certificate.verifyFailures = {
            {.kind = FileTransferProtocol::VerifyFailureKind::SelfSigned, .reason = "self-signed certificate"},
            {.kind = FileTransferProtocol::VerifyFailureKind::Expired, .reason = "certificate has expired"},
        };
        certificate.certificateSha256.fill(0xABu);
        untrusted.untrustedCertificate = certificate;
        const auto message = FtpConnect::loginFailureMessage(untrusted, session);
        EXPECT_NE(message.find("is not trusted (self-signed certificate, certificate has expired)"), std::string::npos)
            << message;
        EXPECT_NE(message.find(FileTransferProtocol::formatDigest(certificate.certificateSha256)), std::string::npos)
            << message;
    }

    /**
     * @brief Connects through FtpConnect to a scripted server, on an FtpEngineContext of two threads.
     */
    class FtpConnectTests : public ::testing::Test
    {
      protected:
        void TearDown() override
        {
            connection_.reset();
            if (!server_)
                return;
            EXPECT_TRUE(server_->waitUntilDisconnected());
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
            if (!HasFailure())
                return;
            for (std::size_t connection = 0u; connection < addedConnections_; ++connection)
            {
                std::string commands{};
                for (auto const& command : server_->commands(connection))
                    commands += fmt::format("\n  {}", command);
                std::cout << fmt::format("Commands of connection {}:{}\n", connection, commands);
            }
        }

        void startServer()
        {
            auto started = FtpConnectTesting::ScriptedFtpServer::start();
            ASSERT_TRUE(started) << started.error();
            server_ = std::move(started).value();
        }

        void addConnection(FtpConnectTesting::Script script)
        {
            server_->addConnection(std::move(script));
            ++addedConnections_;
        }

        /**
         * @brief A plain session on the server that logs in as @p user, nobody for an anonymous one.
         */
        Persistence::FtpSessionOptions session(std::optional<std::string> user = "alice") const
        {
            auto options = FtpConnectTesting::sessionFor(server_->address(), std::move(user));
            options.port = server_->port();
            options.tlsMode = Persistence::FtpTlsMode::Plain;
            return options;
        }

        std::expected<SessionConnection, std::string> connect(
            Persistence::FtpSessionOptions const& options,
            FtpConnect::PasswordSource const& askPassword,
            std::function<void()> onConnectionLoss = {},
            std::stop_token const& stop = {},
            FtpConnect::TrustSource const& askTrust = FtpConnectTesting::neverAskedAboutTrust()
        )
        {
            return FtpConnect::connect(
                engine_, options, askPassword, trustStore_, askTrust, std::move(onConnectionLoss), stop
            );
        }

        /**
         * @brief A session on the server over explicit TLS, which trusts no authority of the tests.
         */
        Persistence::FtpSessionOptions explicitTlsSession(std::optional<std::string> user = "alice") const
        {
            auto options = session(std::move(user));
            options.tlsMode = Persistence::FtpTlsMode::Explicit;
            return options;
        }

        static FileTransferProtocol::Test::TestPki const& pki()
        {
            const auto made = FileTransferProtocol::Test::testPki();
            if (!made)
                throw std::runtime_error{made.error()};
            return made->get();
        }

        /**
         * @brief Keeps @p connected as the connection of the test, starts it and opens a file system on it.
         */
        std::shared_ptr<RemoteIo::IRemoteFileSystem> startAndOpen(SessionConnection connected)
        {
            connection_ = std::move(connected);
            connection_->connection->start();
            auto created = connection_->connection->createFileSystem();
            if (created.wait_for(std::chrono::seconds{10}) != std::future_status::ready)
                throw std::runtime_error{"no file system within 10 s"};
            auto fileSystem = created.get();
            if (!fileSystem)
                throw std::runtime_error{fileSystem.error().toString()};
            return fileSystem->lock();
        }

        static RemoteIo::OperationResult<RemoteIo::ListDirectory>
        list(RemoteIo::IRemoteFileSystem& fileSystem, std::string_view path)
        {
            auto listed = fileSystem.later(
                RemoteIo::ListDirectory{.path = FtpConnectTesting::remotePath(path), .resolveLinks = false}
            );
            if (listed.wait_for(std::chrono::seconds{30}) != std::future_status::ready)
                throw std::runtime_error{fmt::format("listing {} took over 30 s", path)};
            return listed.get();
        }

        FtpEngineContext engine_{2u};
        /** @brief The keys the connects of the test trust, kept for the test only. */
        FtpConnect::CertificateTrustStore trustStore_{};
        std::shared_ptr<FtpConnectTesting::ServedTree> tree_ =
            std::make_shared<FtpConnectTesting::ServedTree>(std::vector<FileTransferProtocol::Test::ServedEntry>{
                FtpConnectTesting::servedDirectory(
                    "dir",
                    {FtpConnectTesting::servedFile("a.txt", "alpha"), FtpConnectTesting::servedFile("b.txt", "beta")}
                ),
                FtpConnectTesting::servedDirectory("slow", {FtpConnectTesting::servedFile("big", "slow")}),
            });
        std::unique_ptr<FtpConnectTesting::ScriptedFtpServer> server_{};
        std::size_t addedConnections_{0u};
        std::optional<SessionConnection> connection_{};
    };

    TEST_F(FtpConnectTests, APasswordFromTheSourceLogsInAndTheSessionHasNoTerminals)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret"));
        std::vector<std::string> prompts{};
        auto connected = connect(
            session(),
            [&prompts](std::string const& prompt) -> std::optional<std::string>
            {
                prompts.push_back(prompt);
                return "s3cret";
            }
        );
        ASSERT_TRUE(connected) << connected.error();
        EXPECT_EQ(prompts, std::vector<std::string>{fmt::format("Password for alice@{}:", server_->address())});
        EXPECT_EQ(connected->terminalHost, nullptr);

        const auto fileSystem = startAndOpen(*std::move(connected));
        const auto listed = list(*fileSystem, "/dir");
        ASSERT_TRUE(listed) << listed.error().toString();
        EXPECT_EQ(listed->size(), 2u);
    }

    TEST_F(FtpConnectTests, AnAnonymousLoginAsksForNoPassword)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(loggingIn(tree_, "anonymous", "anonymous@"));
        auto connected = connect(session(std::nullopt), neverAsked());
        ASSERT_TRUE(connected) << connected.error();
        connection_ = *std::move(connected);
    }

    TEST_F(FtpConnectTests, ACancelledPromptFailsTheConnectBeforeAnyCredentialIsSent)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        const auto connected = connect(
            session(),
            [](std::string const&) -> std::optional<std::string>
            {
                return std::nullopt;
            }
        );
        ASSERT_FALSE(connected);
        EXPECT_EQ(connected.error(), "The password prompt was cancelled.");
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->commands(0u), std::vector<std::string>{"QUIT"}) << "a credential was sent";
    }

    TEST_F(FtpConnectTests, AnUnreachableServerIsNotAskedForAPassword)
    {
        using namespace FtpConnectTesting;
        // A port nobody listens on any more.
        boost::asio::io_context context{};
        boost::asio::ip::tcp::acceptor acceptor{context, {boost::asio::ip::make_address("127.0.0.1"), 0u}};
        auto options = FtpConnectTesting::sessionFor("127.0.0.1", "alice");
        options.port = acceptor.local_endpoint().port();
        options.tlsMode = Persistence::FtpTlsMode::Plain;
        acceptor.close();

        const auto connected = connect(options, neverAsked());

        ASSERT_FALSE(connected);
        EXPECT_NE(connected.error().find("could not be reached"), std::string::npos) << connected.error();
    }

    TEST_F(FtpConnectTests, TheServerIsReachedBeforeThePasswordIsAsked)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret"));
        std::vector<std::string> commandsWhenAsked{};

        auto connected = connect(
            session(),
            [this, &commandsWhenAsked](std::string const&) -> std::optional<std::string>
            {
                commandsWhenAsked = server_->commands(0u);
                return "s3cret";
            }
        );

        ASSERT_TRUE(connected) << connected.error();
        EXPECT_EQ(commandsWhenAsked, std::vector<std::string>{"QUIT"});
    }

    TEST_F(FtpConnectTests, ARefusedLoginFailsWithAReadableMessageWithoutThePassword)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(refusingTheLogin("alice", "s3cret"));
        const auto connected = connect(session(), answering("s3cret"));
        ASSERT_FALSE(connected);
        EXPECT_EQ(
            connected.error(),
            fmt::format(
                "The FTP server {}:{} refused the login of alice: Login incorrect.", server_->address(), server_->port()
            )
        );
        EXPECT_EQ(connected.error().find("s3cret"), std::string::npos);
    }

    TEST_F(FtpConnectTests, AnEmptyAccountIsNoneWhenTheServerAsksForOne)
    {
        using namespace FtpConnectTesting;
        startServer();
        Script script{};
        script.reply("220 Ready")
            .expect("USER", "alice")
            .reply("331 Password required")
            .expect("PASS", "s3cret")
            .reply("332 Need account for login.")
            .expectDisconnect();
        addConnection(reaching());
        addConnection(std::move(script));
        auto options = session();
        options.users.front().account = "";

        const auto connected = connect(options, answering("s3cret"));
        ASSERT_FALSE(connected);
        EXPECT_NE(connected.error().find("refused the login of alice"), std::string::npos) << connected.error();
        EXPECT_NE(connected.error().find("asks for an account"), std::string::npos) << connected.error();
    }

    TEST_F(FtpConnectTests, AStopEndsALoginThatRuns)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(silentAfterTheGreeting());
        auto options = session();
        // The login alone would wait a minute for the reply to USER.
        options.ftpOptions.value().commandTimeoutSeconds = 60;
        std::stop_source stop{};
        auto connecting = std::async(
            std::launch::async,
            [this, &options, &stop]()
            {
                return connect(options, answering("s3cret"), {}, stop.get_token());
            }
        );
        ASSERT_TRUE(server_->waitForCheckpoint("login stalled"));

        const auto start = std::chrono::steady_clock::now();
        stop.request_stop();
        ASSERT_EQ(connecting.wait_for(std::chrono::seconds{20}), std::future_status::ready);
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{20});
        const auto connected = connecting.get();
        ASSERT_FALSE(connected);
        EXPECT_NE(connected.error().find("was stopped"), std::string::npos) << connected.error();
    }

    TEST_F(FtpConnectTests, AStopEndsTheReachOfAServerThatDoesNotGreet)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(silentBeforeTheGreeting());
        auto options = session();
        // The reach alone would wait a minute for the greeting.
        options.ftpOptions.value().connectTimeoutSeconds = 60;
        std::stop_source stop{};
        auto connecting = std::async(
            std::launch::async,
            [this, &options, &stop]()
            {
                return connect(options, neverAsked(), {}, stop.get_token());
            }
        );
        ASSERT_TRUE(server_->waitForCheckpoint("reach stalled"));

        stop.request_stop();
        ASSERT_EQ(connecting.wait_for(std::chrono::seconds{20}), std::future_status::ready);
        const auto connected = connecting.get();
        ASSERT_FALSE(connected);
        EXPECT_NE(connected.error().find("was stopped"), std::string::npos) << connected.error();
        EXPECT_TRUE(server_->waitUntilDisconnected()) << "the reach kept its connection after the stop";
    }

    TEST_F(FtpConnectTests, AStoppedConnectNeitherAsksNorConnects)
    {
        startServer();
        std::stop_source stop{};
        stop.request_stop();
        const auto connected = connect(session(), FtpConnectTesting::neverAsked(), {}, stop.get_token());
        ASSERT_FALSE(connected);
        EXPECT_NE(connected.error().find("was stopped"), std::string::npos) << connected.error();
        EXPECT_TRUE(server_->transcript().empty()) << "connected after the stop";
    }

    TEST_F(FtpConnectTests, AnUntrustedCertificateIsAskedAboutBeforeThePassword)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        addConnection(reachingOverTls(pki().selfSigned));
        const auto prompts = std::make_shared<Prompts>();

        const auto connected = connect(
            explicitTlsSession(),
            decliningThePassword(prompts),
            {},
            {},
            answeringTrust(prompts, FtpConnect::TrustAnswer::Once)
        );
        ASSERT_FALSE(connected);
        EXPECT_EQ(connected.error(), "The password prompt was cancelled.");
        EXPECT_EQ(prompts->order, (std::vector<std::string>{"trust", "password"}));
        ASSERT_EQ(prompts->trustQuestions.size(), 1u);
        auto const& question = prompts->trustQuestions.front();
        EXPECT_EQ(question.host, server_->address());
        EXPECT_EQ(question.port, server_->port());
        EXPECT_FALSE(question.pinnedKeyDiffers);
        EXPECT_FALSE(question.certificate.verifyFailures.empty());
        // The second reach took the certificate by the key the user trusted.
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->commands(1u), (std::vector<std::string>{"AUTH TLS", "QUIT"}));
    }

    TEST_F(FtpConnectTests, ARefusedCertificateEndsTheConnectWithoutAPasswordPrompt)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        const auto prompts = std::make_shared<Prompts>();

        const auto connected = connect(
            explicitTlsSession(), neverAsked(), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Refused)
        );
        ASSERT_FALSE(connected);
        EXPECT_EQ(
            connected.error(),
            fmt::format("The certificate of the FTP server {}:{} was not trusted.", server_->address(), server_->port())
        );
        EXPECT_EQ(prompts->order, (std::vector<std::string>{"trust"}));
    }

    TEST_F(FtpConnectTests, AKeyOtherThanThePinnedOneIsAskedAboutAsChanged)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        const auto options = explicitTlsSession();
        const FileTransferProtocol::Sha256Digest trustedBefore{};
        trustStore_.trustForThisRun(options.host, server_->port(), trustedBefore);
        const auto prompts = std::make_shared<Prompts>();

        const auto connected =
            connect(options, neverAsked(), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Refused));
        ASSERT_FALSE(connected);
        ASSERT_EQ(prompts->trustQuestions.size(), 1u);
        EXPECT_TRUE(prompts->trustQuestions.front().pinnedKeyDiffers);
        const auto trusted = trustStore_.trustedKeyOf(options.host, server_->port());
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, trustedBefore) << "a refused certificate replaced the trusted key";
    }

    TEST_F(FtpConnectTests, AKeyTrustedOnceIsNotAskedAboutAgainInThisRun)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        addConnection(reachingOverTls(pki().selfSigned));
        addConnection(reachingOverTls(pki().selfSigned));
        const auto options = explicitTlsSession();
        const auto prompts = std::make_shared<Prompts>();

        ASSERT_FALSE(connect(
            options, decliningThePassword(prompts), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Once)
        ));
        ASSERT_EQ(prompts->trustQuestions.size(), 1u);
        const auto trusted = trustStore_.trustedKeyOf(options.host, server_->port());
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, prompts->trustQuestions.front().certificate.publicKeySha256);
        EXPECT_FALSE(trusted->lasting);

        // A reconnect: the key of the first connect is pinned, nobody is asked.
        const auto again = connect(options, decliningThePassword(prompts), {}, {}, neverAskedAboutTrust());
        ASSERT_FALSE(again);
        EXPECT_EQ(again.error(), "The password prompt was cancelled.");
        EXPECT_EQ(prompts->order, (std::vector<std::string>{"trust", "password", "password"}));
    }

    TEST_F(FtpConnectTests, AnotherKeyThanTheOneTrustedOnceIsAskedAboutAsChanged)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        addConnection(reachingOverTls(pki().selfSigned));
        addConnection(refusedOverTls(pki().otherAuthority));
        const auto options = explicitTlsSession();
        const auto prompts = std::make_shared<Prompts>();

        ASSERT_FALSE(connect(
            options, decliningThePassword(prompts), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Once)
        ));
        // A reconnect that meets another server, as one that intercepts the connection.
        const auto again =
            connect(options, neverAsked(), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Refused));
        ASSERT_FALSE(again);
        ASSERT_EQ(prompts->trustQuestions.size(), 2u);
        EXPECT_FALSE(prompts->trustQuestions.front().pinnedKeyDiffers);
        EXPECT_TRUE(prompts->trustQuestions.back().pinnedKeyDiffers);
        const auto trusted = trustStore_.trustedKeyOf(options.host, server_->port());
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, prompts->trustQuestions.front().certificate.publicKeySha256);
    }

    TEST_F(FtpConnectTests, AlwaysTrustSavesTheKeyOfTheCertificateTheBackendSaw)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        addConnection(reachingOverTls(pki().selfSigned));
        const auto options = explicitTlsSession();
        const auto prompts = std::make_shared<Prompts>();

        ASSERT_FALSE(connect(
            options, decliningThePassword(prompts), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Always)
        ));
        ASSERT_EQ(prompts->trustQuestions.size(), 1u);
        const auto trusted = trustStore_.trustedKeyOf(options.host, server_->port());
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, prompts->trustQuestions.front().certificate.publicKeySha256);
        EXPECT_TRUE(trusted->lasting);
    }

    TEST_F(FtpConnectTests, ARefusedCertificateTrustsNoKey)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        const auto options = explicitTlsSession();
        const auto prompts = std::make_shared<Prompts>();

        ASSERT_FALSE(connect(options, neverAsked(), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Refused)));
        EXPECT_FALSE(trustStore_.trustedKeyOf(options.host, server_->port()));
    }

    TEST_F(FtpConnectTests, AlwaysTrustingACertificateTheAuthoritiesVouchForForgetsTheTrustedKey)
    {
        using namespace FtpConnectTesting;
        const AuthorityOfTheTests trusted{pki().authority};
        startServer();
        // Refused for its key alone, reached with it, then reached by a later connect.
        addConnection(refusedOverTls(pki().server));
        addConnection(reachingOverTls(pki().server));
        addConnection(reachingOverTls(pki().server));
        const auto options = explicitTlsSession();
        const FileTransferProtocol::Sha256Digest trustedBefore{};
        ASSERT_TRUE(trustStore_.trustLastingly(options.host, server_->port(), trustedBefore));
        const auto prompts = std::make_shared<Prompts>();

        ASSERT_FALSE(connect(
            options, decliningThePassword(prompts), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Always)
        ));
        ASSERT_EQ(prompts->trustQuestions.size(), 1u);
        auto const& question = prompts->trustQuestions.front();
        EXPECT_TRUE(question.pinnedKeyDiffers);
        EXPECT_TRUE(question.certificate.verifyFailures.empty())
            << FileTransferProtocol::reasonsOf(question.certificate.verifyFailures);
        EXPECT_FALSE(trustStore_.trustedKeyOf(options.host, server_->port())) << "a key is still pinned";

        // The authorities are trusted for the server again, nobody is asked.
        const auto again = connect(options, decliningThePassword(prompts), {}, {}, neverAskedAboutTrust());
        ASSERT_FALSE(again);
        EXPECT_EQ(again.error(), "The password prompt was cancelled.");
        EXPECT_EQ(prompts->order, (std::vector<std::string>{"trust", "password", "password"}));
    }

    TEST_F(FtpConnectTests, TrustingACertificateTheAuthoritiesVouchForOnceKeepsTheSavedKey)
    {
        using namespace FtpConnectTesting;
        const AuthorityOfTheTests trusted{pki().authority};
        startServer();
        addConnection(refusedOverTls(pki().server));
        addConnection(reachingOverTls(pki().server));
        const auto options = explicitTlsSession();
        const FileTransferProtocol::Sha256Digest trustedBefore{};
        ASSERT_TRUE(trustStore_.trustLastingly(options.host, server_->port(), trustedBefore));
        const auto prompts = std::make_shared<Prompts>();

        ASSERT_FALSE(connect(
            options, decliningThePassword(prompts), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Once)
        ));
        ASSERT_EQ(prompts->trustQuestions.size(), 1u);
        // For this run the key of the certificate the user saw, the saved one comes back with the next run.
        const auto now = trustStore_.trustedKeyOf(options.host, server_->port());
        ASSERT_TRUE(now);
        EXPECT_EQ(now->publicKey, prompts->trustQuestions.front().certificate.publicKeySha256);
        EXPECT_FALSE(now->lasting);
    }

    TEST_F(FtpConnectTests, ACertificateThatChangedWhileItWasAskedAboutFailsWithItsFingerprint)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        addConnection(refusedOverTls(pki().otherAuthority));
        const auto prompts = std::make_shared<Prompts>();

        const auto connected =
            connect(explicitTlsSession(), neverAsked(), {}, {}, answeringTrust(prompts, FtpConnect::TrustAnswer::Once));
        ASSERT_FALSE(connected);
        EXPECT_EQ(prompts->order, (std::vector<std::string>{"trust"})) << "the changed certificate was asked about";
        // The key that is trusted is the one the user was shown, the changed certificate got none in.
        const auto trusted = trustStore_.trustedKeyOf(server_->address(), server_->port());
        ASSERT_TRUE(trusted);
        EXPECT_EQ(trusted->publicKey, prompts->trustQuestions.front().certificate.publicKeySha256);
        EXPECT_NE(connected.error().find("is not trusted"), std::string::npos) << connected.error();
        const std::regex fingerprint{"SHA-256 fingerprint is ([0-9A-F]{2}:){31}[0-9A-F]{2}\\."};
        EXPECT_TRUE(std::regex_search(connected.error(), fingerprint)) << connected.error();
    }

    TEST_F(FtpConnectTests, AnAnonymousLoginOverTlsIsAskedAboutTheCertificate)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        const auto prompts = std::make_shared<Prompts>();

        const auto connected = connect(
            explicitTlsSession(std::nullopt),
            neverAsked(),
            {},
            {},
            answeringTrust(prompts, FtpConnect::TrustAnswer::Refused)
        );
        ASSERT_FALSE(connected);
        EXPECT_EQ(prompts->order, (std::vector<std::string>{"trust"}));
        EXPECT_NE(connected.error().find("was not trusted"), std::string::npos) << connected.error();
    }

    TEST_F(FtpConnectTests, AStopDuringTheTrustPromptEndsTheConnect)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        std::stop_source stop{};
        const auto askTrust = [&stop](FtpConnect::TrustQuestion const&)
        {
            // Shutdown ends a prompt that waits, after it stopped the connects.
            stop.request_stop();
            return FtpConnect::TrustAnswer::Once;
        };

        const auto connected = connect(explicitTlsSession(), neverAsked(), {}, stop.get_token(), askTrust);
        ASSERT_FALSE(connected);
        EXPECT_NE(connected.error().find("was stopped"), std::string::npos) << connected.error();
        const auto transcript = server_->transcript();
        const auto reaches = std::ranges::count_if(
            transcript,
            [](FileTransferProtocol::Test::TranscriptEntry const& entry)
            {
                return entry.kind == FileTransferProtocol::Test::TranscriptEntry::Kind::Connected;
            }
        );
        EXPECT_EQ(reaches, 1) << "reached again after the stop";
    }

    TEST_F(FtpConnectTests, ALostSessionIsReportedOnce)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret", resetsAtTheFirstListing));
        // The logins in the background after the loss.
        addConnection(refusingTheLogin("alice", "s3cret"));
        addConnection(refusingTheLogin("alice", "s3cret"));
        const auto losses = std::make_shared<CountedLosses>();
        auto connected = connect(
            session(),
            answering("s3cret"),
            [losses]()
            {
                losses->record();
            }
        );
        ASSERT_TRUE(connected) << connected.error();
        const auto fileSystem = startAndOpen(*std::move(connected));
        EXPECT_FALSE(list(*fileSystem, "/dir"));

        ASSERT_EQ(losses->first.get_future().wait_for(std::chrono::seconds{20}), std::future_status::ready);
        const auto afterwards = list(*fileSystem, "/dir");
        ASSERT_FALSE(afterwards);
        EXPECT_EQ(afterwards.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << afterwards.error().toString();
        connection_.reset();
        EXPECT_EQ(losses->count.load(), 1);
    }

    TEST_F(FtpConnectTests, RemovingTheSessionEndsTheListingThatRunsWithoutALoss)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret", stallsTheSlowListing));
        // C0 closed by the cancel of the listing may be logged in again in the background before the pool closes.
        addConnection(spare(tree_));
        addConnection(spare(tree_));
        const auto losses = std::make_shared<CountedLosses>();
        auto connected = connect(
            session(),
            answering("s3cret"),
            [losses]()
            {
                losses->record();
            }
        );
        ASSERT_TRUE(connected) << connected.error();
        const auto fileSystem = startAndOpen(*std::move(connected));
        auto listing = fileSystem->later(RemoteIo::ListDirectory{.path = remotePath("/slow"), .resolveLinks = false});
        ASSERT_TRUE(server_->waitForCheckpoint("listing stalled"));

        // As Session::stop does: the file system first, then the connection.
        const auto start = std::chrono::steady_clock::now();
        fileSystem->close();
        connection_.reset();
        // The operations' backstop is 30 s: a removal that waited for the listing would take that long.
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{20});
        ASSERT_EQ(listing.wait_for(std::chrono::seconds{0}), std::future_status::ready)
            << "the listing outlived its session";
        const auto listed = listing.get();
        ASSERT_FALSE(listed);
        EXPECT_EQ(listed.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << listed.error().toString();
        EXPECT_EQ(losses->count.load(), 0);
    }

    TEST_F(FtpConnectTests, TheFileSystemTellsWhetherTheServerCanCheckUploadDestinations)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret"));
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret", listsNoSize));

        auto withSize = connect(session(), answering("s3cret"));
        ASSERT_TRUE(withSize) << withSize.error();
        EXPECT_TRUE(startAndOpen(*std::move(withSize))->capabilities().probeEntries);
        connection_.reset();

        auto withoutSize = connect(session(), answering("s3cret"));
        ASSERT_TRUE(withoutSize) << withoutSize.error();
        EXPECT_FALSE(startAndOpen(*std::move(withoutSize))->capabilities().probeEntries);
    }

    /**
     * @brief FTP sessions through the SessionManager, with password providers and its shutdown. Needs a window.
     */
    class FtpSessionManagerTests : public FtpConnectTests
    {
      protected:
        struct Connected
        {
            std::optional<Ids::SessionId> id{};
            std::optional<std::string> error{};
        };

        void SetUp() override
        {
            if (!NuiEnv::instance().available())
                GTEST_SKIP() << "No display available";
            auto& env = NuiEnv::instance();
            manager_ = std::make_shared<SessionManager>(
                strandPool_.get_executor(), stateHolder_, *env.window, *env.hub, engine_
            );
        }

        void TearDown() override
        {
            if (manager_)
                manager_->shutdown();
            manager_.reset();
            FtpConnectTests::TearDown();
        }

        /**
         * @brief Adds a session through @p connect, the result comes once the connect ended.
         */
        std::future<Connected> startConnect(SessionManager::Connect connect, Persistence::SftpOptions transfers)
        {
            auto completed = std::make_shared<std::promise<Connected>>();
            auto future = completed->get_future();
            manager_->addSession(
                std::move(connect),
                std::move(transfers),
                [completed](std::optional<Ids::SessionId> const& id, std::optional<std::string> const& error)
                {
                    completed->set_value({.id = id, .error = error});
                }
            );
            return future;
        }

        static Connected awaitConnect(std::future<Connected>& connecting)
        {
            if (connecting.wait_for(std::chrono::seconds{60}) != std::future_status::ready)
                throw std::runtime_error{"the connect did not complete within 60 s"};
            return connecting.get();
        }

        Connected connectThroughManager(Persistence::FtpSessionOptions options)
        {
            auto transfers = options.sftpOptions.value();
            auto connecting = startConnect(manager_->ftpConnect(std::move(options)), std::move(transfers));
            return awaitConnect(connecting);
        }

        boost::asio::thread_pool strandPool_{2};
        Persistence::StateHolder stateHolder_{};
        std::shared_ptr<SessionManager> manager_{};
    };

    TEST_F(FtpSessionManagerTests, AnFtpSessionAsksTheProvidersForThePassword)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret"));
        AnsweringProvider provider{"s3cret"};
        manager_->addPasswordProvider(0, &provider);

        const auto connected = connectThroughManager(session());
        ASSERT_TRUE(connected.id) << connected.error.value_or("no error");
        const auto questions = provider.questions();
        ASSERT_EQ(questions.size(), 1u);
        EXPECT_EQ(questions.front().whatFor, "password");
        EXPECT_EQ(questions.front().prompt, fmt::format("Password for alice@{}:", server_->address()));

        manager_->shutdown();
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->commands(0u).back(), "QUIT");
    }

    TEST_F(FtpSessionManagerTests, ADeclinedPromptFailsTheConnect)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        AnsweringProvider provider{std::nullopt};
        manager_->addPasswordProvider(0, &provider);

        const auto connected = connectThroughManager(session());
        EXPECT_FALSE(connected.id);
        EXPECT_EQ(connected.error, "The password prompt was cancelled.");
        EXPECT_EQ(provider.questions().size(), 1u);
    }

    TEST_F(FtpSessionManagerTests, AnUntrustedCertificateIsPutToTheTrustProviderBeforeThePassword)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        addConnection(reachingOverTls(pki().selfSigned));
        AnsweringTrustProvider trust{AnsweringTrustProvider::Answer::Trust};
        manager_->setTrustProvider(&trust);
        AnsweringProvider passwords{std::nullopt};
        manager_->addPasswordProvider(0, &passwords);

        const auto connected = connectThroughManager(explicitTlsSession());
        EXPECT_FALSE(connected.id);
        EXPECT_EQ(connected.error, "The password prompt was cancelled.");
        ASSERT_EQ(trust.questions().size(), 1u);
        EXPECT_EQ(trust.questions().front().host, server_->address());
        EXPECT_EQ(passwords.questions().size(), 1u);
    }

    TEST_F(FtpSessionManagerTests, ARefusedTrustPromptCancelsTheConnect)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        AnsweringTrustProvider trust{AnsweringTrustProvider::Answer::Refuse};
        manager_->setTrustProvider(&trust);
        auto options = explicitTlsSession();
        auto transfers = options.sftpOptions.value();
        const auto declined = std::make_shared<bool>(false);

        auto connecting = startConnect(
            manager_->noticingDeclinedPasswords(manager_->ftpConnect(std::move(options)), declined),
            std::move(transfers)
        );
        const auto connected = awaitConnect(connecting);
        EXPECT_FALSE(connected.id);
        EXPECT_TRUE(*declined) << "a refused certificate reads as an error";
    }

    TEST_F(FtpSessionManagerTests, WithoutATrustProviderAnUntrustedCertificateEndsTheConnect)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));

        const auto connected = connectThroughManager(explicitTlsSession());
        EXPECT_FALSE(connected.id);
        ASSERT_TRUE(connected.error);
        EXPECT_NE(connected.error->find("was not trusted"), std::string::npos) << *connected.error;
    }

    TEST_F(FtpSessionManagerTests, TheShutdownEndsAWaitingTrustPrompt)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(refusedOverTls(pki().selfSigned));
        AnsweringTrustProvider trust{AnsweringTrustProvider::Answer::Never};
        manager_->setTrustProvider(&trust);
        auto asked = trust.firstAsked();
        auto options = explicitTlsSession();
        auto transfers = options.sftpOptions.value();

        auto connecting = startConnect(manager_->ftpConnect(std::move(options)), std::move(transfers));
        ASSERT_EQ(asked.wait_for(std::chrono::seconds{20}), std::future_status::ready);
        manager_->shutdown();

        const auto connected = awaitConnect(connecting);
        EXPECT_FALSE(connected.id);
        ASSERT_TRUE(connected.error);
        EXPECT_NE(connected.error->find("was stopped"), std::string::npos) << *connected.error;
    }

    TEST_F(FtpSessionManagerTests, TheShutdownClosesTheFtpSessionsAndRefusesNewOnes)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(loggingIn(tree_, "anonymous", "anonymous@"));
        AnsweringProvider provider{"s3cret"};
        manager_->addPasswordProvider(0, &provider);

        const auto connected = connectThroughManager(session(std::nullopt));
        ASSERT_TRUE(connected.id) << connected.error.value_or("no error");
        EXPECT_TRUE(provider.questions().empty()) << "an anonymous login asked for a password";

        manager_->shutdown();
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->commands(0u).back(), "QUIT");

        const auto refused = connectThroughManager(session(std::nullopt));
        EXPECT_FALSE(refused.id);
        ASSERT_TRUE(refused.error);
        EXPECT_NE(refused.error->find("closing"), std::string::npos) << *refused.error;
    }

    TEST_F(FtpSessionManagerTests, ALossBeforeTheStartRemovesTheSessionAfterItStarted)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret", resetsAtTheFirstListing));
        // The logins in the background after the loss.
        addConnection(refusingTheLogin("alice", "s3cret"));
        addConnection(refusingTheLogin("alice", "s3cret"));
        AnsweringProvider provider{"s3cret"};
        manager_->addPasswordProvider(0, &provider);

        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem{};
        const auto lossReported = std::make_shared<std::promise<void>>();
        auto lossReport = lossReported->get_future();
        auto loseBeforeTheStart = [&fileSystem, lossReported, connect = manager_->ftpConnect(session())](
                                      std::function<void()> onConnectionLoss
                                  ) -> std::expected<SessionConnection, std::string>
        {
            auto connected = connect(
                [onConnectionLoss = std::move(onConnectionLoss), lossReported]()
                {
                    onConnectionLoss();
                    lossReported->set_value();
                }
            );
            if (!connected)
                return connected;
            auto created = connected->connection->createFileSystem().get();
            if (!created)
                return std::unexpected(created.error().toString());
            fileSystem = created->lock();
            EXPECT_FALSE(list(*fileSystem, "/dir"));
            // A lost session refuses at once.
            const auto lost = list(*fileSystem, "/dir");
            EXPECT_FALSE(lost);
            return connected;
        };
        auto completed = std::make_shared<std::promise<Connected>>();
        auto future = completed->get_future();
        manager_->addSession(
            std::move(loseBeforeTheStart),
            {},
            [completed](std::optional<Ids::SessionId> const& id, std::optional<std::string> const& error)
            {
                completed->set_value({.id = id, .error = error});
            }
        );
        ASSERT_EQ(future.wait_for(std::chrono::seconds{60}), std::future_status::ready);
        const auto connected = future.get();
        ASSERT_TRUE(connected.id) << connected.error.value_or("no error");
        ASSERT_TRUE(fileSystem);
        // The engine may tell the session of the loss only after its start, under load.
        ASSERT_EQ(lossReport.wait_for(std::chrono::seconds{20}), std::future_status::ready);

        // Behind the removal of the lost session on the strand, which the report posted.
        const auto refused = connectThroughManager(Persistence::FtpSessionOptions{});
        EXPECT_FALSE(refused.id);
        EXPECT_FALSE(fileSystem->close()) << "the lost session was not removed, its file system is still open";
    }

    TEST_F(FtpSessionManagerTests, AConnectCallForTheFtpEngineLogsInToTheFtpServer)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(loggingIn(tree_, "alice", "s3cret"));
        AnsweringProvider provider{"s3cret"};
        manager_->addPasswordProvider(0, &provider);
        auto options = session();
        options.sftpOptions.value().concurrency = 2;
        const nlohmann::json parameters{{"engineType", "ftp"}, {"engineOptions", options}, {"showFailureToast", false}};
        const auto passwordDeclined = std::make_shared<bool>(true);

        auto request = manager_->connectRequestOf(parameters, passwordDeclined);
        ASSERT_TRUE(request) << request.error();
        EXPECT_EQ(request->engine, Persistence::TerminalEngineType::ftp);
        EXPECT_EQ(request->sftpOptions.concurrency, 2);
        auto connecting = startConnect(std::move(request->connect), std::move(request->sftpOptions));
        const auto connected = awaitConnect(connecting);
        ASSERT_TRUE(connected.id) << connected.error.value_or("no error");
        EXPECT_FALSE(*passwordDeclined);
        const auto questions = provider.questions();
        ASSERT_EQ(questions.size(), 1u);
        EXPECT_EQ(questions.front().prompt, fmt::format("Password for alice@{}:", server_->address()));
    }

    TEST_F(FtpSessionManagerTests, AConnectCallWithoutAnEngineOrOptionsToConnectWithIsRefused)
    {
        const auto passwordDeclined = std::make_shared<bool>(false);
        const auto refusalOf = [this, &passwordDeclined](nlohmann::json const& parameters)
        {
            const auto request = manager_->connectRequestOf(parameters, passwordDeclined);
            if (request)
                return std::string{};
            return request.error();
        };
        EXPECT_NE(refusalOf({{"engineType", "ftp"}}).find("engineOptions"), std::string::npos);
        EXPECT_NE(
            refusalOf({{"engineType", "ftp"}, {"engineOptions", "not options"}}).find("no object"), std::string::npos
        );
        EXPECT_NE(
            refusalOf({{"engineType", "shell"}, {"engineOptions", Persistence::FtpSessionOptions{}}}).find("shell"),
            std::string::npos
        );

        // Without an engine type a connect is SSH, as before FTP.
        const auto secureShell =
            manager_->connectRequestOf({{"engineOptions", Persistence::SshSessionOptions{}}}, passwordDeclined);
        ASSERT_TRUE(secureShell) << secureShell.error();
        EXPECT_EQ(secureShell->engine, Persistence::TerminalEngineType::ssh);
        EXPECT_TRUE(secureShell->connect);
    }

    TEST_F(FtpSessionManagerTests, TheShutdownStopsAnFtpLoginThatRuns)
    {
        using namespace FtpConnectTesting;
        startServer();
        addConnection(reaching());
        addConnection(silentAfterTheGreeting());
        AnsweringProvider provider{"s3cret"};
        manager_->addPasswordProvider(0, &provider);
        auto options = session();
        // The login alone would wait a minute for the reply to USER.
        options.ftpOptions.value().commandTimeoutSeconds = 60;
        auto transfers = options.sftpOptions.value();
        auto connecting = startConnect(manager_->ftpConnect(std::move(options)), std::move(transfers));
        ASSERT_TRUE(server_->waitForCheckpoint("login stalled"));

        const auto start = std::chrono::steady_clock::now();
        manager_->shutdown();
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{20});
        // The shutdown ran the strand empty, the connect included.
        ASSERT_EQ(connecting.wait_for(std::chrono::seconds{0}), std::future_status::ready);
        const auto connected = connecting.get();
        EXPECT_FALSE(connected.id);
        ASSERT_TRUE(connected.error);
        EXPECT_NE(connected.error->find("was stopped"), std::string::npos) << *connected.error;
    }

    TEST(ConnectRequestTests, AConnectIsSshWithoutAnEngineTypeAndElseSshOrFtp)
    {
        using Persistence::TerminalEngineType;
        const auto engineOf = [](nlohmann::json const& parameters) -> std::optional<TerminalEngineType>
        {
            const auto engine = connectedEngineOf(parameters);
            if (!engine)
                return std::nullopt;
            return *engine;
        };
        EXPECT_EQ(engineOf(nlohmann::json::object()), TerminalEngineType::ssh);
        EXPECT_EQ(engineOf({{"engineType", "ssh"}}), TerminalEngineType::ssh);
        EXPECT_EQ(engineOf({{"engineType", "ftp"}}), TerminalEngineType::ftp);
        for (
            auto const& refused : std::vector<nlohmann::json>{
                {{"engineType", "shell"}},
                {{"engineType", "telnet"}},
                {{"engineType", 42}},
                {{"engineType", nullptr}},
            }
        )
        {
            const auto engine = connectedEngineOf(refused);
            ASSERT_FALSE(engine) << refused.dump();
            EXPECT_FALSE(engine.error().empty());
        }
    }
}
