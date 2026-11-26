#pragma once

#include "nui_env.hpp"
#include "real_server_tests.hpp"

#include <backend/ftp/ftp_engine_context.hpp>
#include <backend/password/password_provider.hpp>
#include <backend/session_manager.hpp>
#include <backend/ssh/host_key_provider.hpp>
#include <paths/native.hpp>
#include <persistence/state_holder.hpp>

#include <boost/asio/thread_pool.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Test
{
    namespace SessionManagerHostKeyTesting
    {
        /**
         * @brief Answers host key prompts as it was told, and keeps what it was asked.
         */
        class AnsweringHostKeyProvider : public HostKeyProvider
        {
          public:
            /**
             * @param answer Nothing leaves the prompt waiting.
             */
            explicit AnsweringHostKeyProvider(std::optional<SecureShell::HostKeyAnswer> answer)
                : answer_{answer}
            {}

            void askAboutHostKey(
                SecureShell::HostKeyQuestion const& question,
                std::function<void(SecureShell::HostKeyAnswer)> onAnswer
            ) override
            {
                {
                    std::scoped_lock lock{mutex_};
                    questions_.push_back(question);
                    if (questions_.size() == 1u)
                        firstAsked_.set_value();
                }
                if (answer_)
                    onAnswer(*answer_);
            }

            std::vector<SecureShell::HostKeyQuestion> questions() const
            {
                std::scoped_lock lock{mutex_};
                return questions_;
            }

            std::future<void> firstAsked()
            {
                return firstAsked_.get_future();
            }

          private:
            const std::optional<SecureShell::HostKeyAnswer> answer_;
            mutable std::mutex mutex_{};
            std::vector<SecureShell::HostKeyQuestion> questions_{};
            std::promise<void> firstAsked_{};
        };

        /**
         * @brief Gives no password, as a cancelled prompt does, and counts the prompts.
         */
        class DecliningPasswordProvider : public PasswordProvider
        {
          public:
            void getPassword(
                std::string const&,
                std::string const&,
                std::function<void(std::optional<std::string>)> const& onPasswordReady
            ) override
            {
                ++asked;
                onPasswordReady(std::nullopt);
            }

            std::size_t asked{0u};
        };
    }

    /**
     * @brief SSH connects of the SessionManager to the mock server, whose host key no known hosts file holds.
     */
    class SessionManagerHostKeyTests : public RealServerTests
    {
      protected:
        struct Connected
        {
            std::optional<Ids::SessionId> id{};
            std::optional<std::string> error{};
            /** @brief Whether the connect counts as cancelled by the user. */
            bool declined{false};
        };

        void SetUp() override
        {
            RealServerTests::SetUp();
            if (!NuiEnv::instance().available())
                GTEST_SKIP() << "No display available";
            auto& env = NuiEnv::instance();
            manager_ = std::make_shared<SessionManager>(
                strandPool_.get_executor(), stateHolder_, *env.window, *env.hub, engine_
            );
            auto [server, thread] = createServer(sftpServer);
            server_ = std::move(server);
            serverThread_ = std::move(thread);
            ASSERT_TRUE(server_);
            manager_->addPasswordProvider(0, &passwords_);
        }

        void TearDown() override
        {
            if (manager_)
                manager_->shutdown();
            manager_.reset();
            if (server_)
                server_->command("exit");
            if (serverThread_.joinable())
                serverThread_.join();
        }

        std::future<Connected> startConnect(bool strict)
        {
            auto options = getSessionOptions(server_->port);
            if (knownHostsFile_)
                options.sshOptions->knownHostsFile = *knownHostsFile_;
            options.sshOptions->strictHostKeyCheck = strict;
            options.sshOptions->bypassConfig = true;
            auto declined = std::make_shared<bool>(false);
            auto completed = std::make_shared<std::promise<Connected>>();
            auto future = completed->get_future();
            manager_->addSession(
                manager_->noticingDeclinedPasswords(manager_->secureShellConnect(std::move(options)), declined),
                {},
                [completed, declined](std::optional<Ids::SessionId> const& id, std::optional<std::string> const& error)
                {
                    completed->set_value({.id = id, .error = error, .declined = *declined});
                }
            );
            return future;
        }

        static Connected awaitConnect(std::future<Connected>& connecting)
        {
            if (connecting.wait_for(std::chrono::seconds{30}) != std::future_status::ready)
                throw std::runtime_error{"the connect did not complete within 30 s"};
            return connecting.get();
        }

        /**
         * @brief Makes a key of no server the one the known hosts file holds for the mock server.
         */
        void knowServerByAnotherKey()
        {
            std::ofstream file{isolateDirectory_.path() / "known_hosts", std::ios_base::binary};
            file << "[127.0.0.1]:" << server_->port
                 << " ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGA+4IwqE35ZvRFPzGpbwCdngq4Z86TtGF75oeyGiqWZ\n";
        }

        /**
         * @brief Gives the connects a known hosts file that takes no key: no directory can be made where a file is.
         */
        void keepTheKnownHostsFileFromTakingKeys()
        {
            const auto blocker = isolateDirectory_.path() / "blocker";
            std::ofstream{blocker} << "a file";
            knownHostsFile_ = blocker / "known_hosts";
        }

        Connected connect(bool strict = false)
        {
            auto connecting = startConnect(strict);
            return awaitConnect(connecting);
        }

        /**
         * @brief What the host key source of the manager answered, and whether its connect counts as cancelled.
         */
        struct Asked
        {
            SecureShell::HostKeyAnswer answer{SecureShell::HostKeyAnswer::Refused};
            bool declined{false};
        };

        /**
         * @brief Puts @p question to the host key source of the manager, on its strand as a connect does.
         */
        Asked ask(SecureShell::HostKeyQuestion question)
        {
            auto declined = std::make_shared<bool>(false);
            auto answer = std::make_shared<SecureShell::HostKeyAnswer>(SecureShell::HostKeyAnswer::Refused);
            auto completed = std::make_shared<std::promise<Asked>>();
            auto asked = completed->get_future();
            manager_->addSession(
                manager_->noticingDeclinedPasswords(
                    [source = manager_->hostKeySource(), question = std::move(question), answer](std::function<void()>)
                        -> std::expected<SessionConnection, std::string>
                    {
                        *answer = source(question);
                        return std::unexpected{std::string{"only asked"}};
                    },
                    declined
                ),
                {},
                [completed, declined, answer](std::optional<Ids::SessionId> const&, std::optional<std::string> const&)
                {
                    completed->set_value({.answer = *answer, .declined = *declined});
                }
            );
            if (asked.wait_for(std::chrono::seconds{30}) != std::future_status::ready)
                throw std::runtime_error{"the question was not answered within 30 s"};
            return asked.get();
        }

        static SecureShell::HostKeyQuestion unknownKeyOf(std::string fingerprint)
        {
            return {
                .problem = SecureShell::HostKeyProblem::Unknown,
                .host = "server.example",
                .port = 22,
                .keyType = "ssh-ed25519",
                .fingerprint = std::move(fingerprint),
            };
        }

        boost::asio::thread_pool strandPool_{2};
        Persistence::StateHolder stateHolder_{};
        FtpEngineContext engine_{1u};
        SessionManagerHostKeyTesting::DecliningPasswordProvider passwords_{};
        std::shared_ptr<SessionManager> manager_{};
        std::shared_ptr<SecureShell::Test::NodeProcessResult> server_{};
        std::thread serverThread_{};
        /** @brief In place of the one of the session options. */
        std::optional<std::filesystem::path> knownHostsFile_{};
    };

    TEST_F(SessionManagerHostKeyTests, AnUnknownHostKeyIsPutToTheHostKeyProviderBeforeThePassword)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);

        const auto connected = connect();

        EXPECT_FALSE(connected.id);
        const auto questions = hostKeys.questions();
        ASSERT_EQ(questions.size(), 1u);
        EXPECT_EQ(questions.front().host, "127.0.0.1");
        EXPECT_EQ(questions.front().port, server_->port);
        EXPECT_EQ(questions.front().keyType, "ssh-rsa");
        EXPECT_EQ(questions.front().fingerprint, "SHA256:YLyQlbOxr7jb8n53bx2ESk4whtSeG9qQgBfHxR/q5Go");
        EXPECT_EQ(passwords_.asked, 1u);
        EXPECT_TRUE(connected.declined) << "the declined password reads as an error";
    }

    TEST_F(SessionManagerHostKeyTests, AHostKeyTheKnownHostsFileDidNotTakeIsNotAskedAboutAgainInThisRun)
    {
        keepTheKnownHostsFileFromTakingKeys();
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);

        connect();
        connect();

        EXPECT_EQ(hostKeys.questions().size(), 1u);
        EXPECT_EQ(passwords_.asked, 2u);
    }

    TEST_F(SessionManagerHostKeyTests, ASavedHostKeyIsNotAskedAboutAgain)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);

        connect();
        connect();

        EXPECT_EQ(hostKeys.questions().size(), 1u);
        EXPECT_EQ(passwords_.asked, 2u);
        EXPECT_TRUE(std::filesystem::exists(isolateDirectory_.path() / "known_hosts"));
    }

    TEST_F(SessionManagerHostKeyTests, ARefusedHostKeyIsAskedAboutAgain)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Refused};
        manager_->setHostKeyProvider(&hostKeys);

        connect();
        connect();

        EXPECT_EQ(hostKeys.questions().size(), 2u);
    }

    TEST_F(SessionManagerHostKeyTests, ARefusedHostKeyCancelsTheConnectBeforeThePasswordIsAsked)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Refused};
        manager_->setHostKeyProvider(&hostKeys);

        const auto connected = connect();

        EXPECT_FALSE(connected.id);
        EXPECT_EQ(passwords_.asked, 0u);
        EXPECT_TRUE(connected.declined) << "a refused host key reads as an error";
    }

    TEST_F(SessionManagerHostKeyTests, AStrictConnectToAnUnknownHostFailsWithoutAnyPrompt)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);

        const auto connected = connect(true);

        EXPECT_FALSE(connected.id);
        EXPECT_TRUE(hostKeys.questions().empty());
        EXPECT_EQ(passwords_.asked, 0u);
        EXPECT_FALSE(connected.declined) << "the refusal is not reported";
        ASSERT_TRUE(connected.error);
        EXPECT_NE(connected.error->find("Strict Host Key Verification"), std::string::npos) << *connected.error;
    }

    TEST_F(SessionManagerHostKeyTests, AChangedHostKeyIsPutToTheProviderAndRefusedWhateverItAnswers)
    {
        knowServerByAnotherKey();
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);

        for (const auto strict : {true, false})
        {
            const auto connected = connect(strict);
            EXPECT_FALSE(connected.id);
            EXPECT_TRUE(connected.declined) << "the warning was shown, an error beside it says it twice";
        }

        // Also the second time, a warning is never taken as trusted for this run.
        const auto questions = hostKeys.questions();
        ASSERT_EQ(questions.size(), 2u);
        EXPECT_EQ(questions.front().problem, SecureShell::HostKeyProblem::OtherType);
        EXPECT_EQ(questions.front().knownHostsFile, Paths::nativeToUtf8(isolateDirectory_.path() / "known_hosts"));
        EXPECT_EQ(passwords_.asked, 0u);
    }

    TEST_F(SessionManagerHostKeyTests, WithoutAHostKeyProviderAnUnknownHostKeyEndsTheConnect)
    {
        const auto connected = connect();

        EXPECT_FALSE(connected.id);
        EXPECT_EQ(passwords_.asked, 0u);
        ASSERT_TRUE(connected.error);
        EXPECT_NE(connected.error->find("was not trusted"), std::string::npos) << *connected.error;
        EXPECT_FALSE(connected.declined) << "nobody saw a prompt, the error is the report";
    }

    TEST_F(SessionManagerHostKeyTests, AnotherUnknownKeyThanTheOneTrustedInThisRunIsShownAsChangedAndRefused)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);
        ASSERT_EQ(ask(unknownKeyOf("SHA256:first")).answer, SecureShell::HostKeyAnswer::Always);

        // The provider would trust it, as it would any prompt.
        const auto asked = ask(unknownKeyOf("SHA256:second"));

        EXPECT_EQ(asked.answer, SecureShell::HostKeyAnswer::Refused);
        EXPECT_TRUE(asked.declined) << "the warning was shown, an error beside it says it twice";
        const auto questions = hostKeys.questions();
        ASSERT_EQ(questions.size(), 2u);
        EXPECT_EQ(questions.back().problem, SecureShell::HostKeyProblem::ChangedSinceTrusted);
        EXPECT_EQ(questions.back().fingerprint, "SHA256:second");
        EXPECT_EQ(questions.back().trustedKey, "ssh-ed25519 SHA256:first");
    }

    TEST_F(SessionManagerHostKeyTests, AKeyTrustedInThisRunCountsForItsServerHoweverTheHostIsCased)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);
        ask(unknownKeyOf("SHA256:first"));
        auto sameKey = unknownKeyOf("SHA256:first");
        sameKey.host = "Server.EXAMPLE";
        auto otherKey = unknownKeyOf("SHA256:second");
        otherKey.host = "SERVER.example";

        EXPECT_EQ(ask(sameKey).answer, SecureShell::HostKeyAnswer::Once) << "trusted, and not saved a second time";
        EXPECT_EQ(ask(otherKey).answer, SecureShell::HostKeyAnswer::Refused);

        const auto questions = hostKeys.questions();
        ASSERT_EQ(questions.size(), 2u) << "the first prompt and the warning";
        EXPECT_EQ(questions.back().problem, SecureShell::HostKeyProblem::ChangedSinceTrusted);
        EXPECT_EQ(questions.back().host, "SERVER.example") << "shown as the user wrote it";
    }

    TEST_F(SessionManagerHostKeyTests, TheKeyTrustedInThisRunStaysTrustedAfterAnotherKeyWasRefused)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);
        ask(unknownKeyOf("SHA256:first"));
        ask(unknownKeyOf("SHA256:second"));

        EXPECT_EQ(ask(unknownKeyOf("SHA256:first")).answer, SecureShell::HostKeyAnswer::Once);
        EXPECT_EQ(ask(unknownKeyOf("SHA256:second")).answer, SecureShell::HostKeyAnswer::Refused);

        // The warning every time, the trusted key not again.
        EXPECT_EQ(hostKeys.questions().size(), 3u);
    }

    TEST_F(SessionManagerHostKeyTests, AKeyTrustedInThisRunForOneServerSaysNothingAboutAnother)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Always};
        manager_->setHostKeyProvider(&hostKeys);
        ask(unknownKeyOf("SHA256:first"));
        auto otherPort = unknownKeyOf("SHA256:second");
        otherPort.port = 2222;

        EXPECT_EQ(ask(otherPort).answer, SecureShell::HostKeyAnswer::Always);

        EXPECT_EQ(hostKeys.questions().back().problem, SecureShell::HostKeyProblem::Unknown);
    }

    TEST_F(SessionManagerHostKeyTests, ARefusedUnknownKeyLeavesNoTrustToCompareWith)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{SecureShell::HostKeyAnswer::Refused};
        manager_->setHostKeyProvider(&hostKeys);
        ask(unknownKeyOf("SHA256:first"));

        ask(unknownKeyOf("SHA256:second"));

        EXPECT_EQ(hostKeys.questions().back().problem, SecureShell::HostKeyProblem::Unknown);
    }

    TEST_F(SessionManagerHostKeyTests, TheShutdownEndsAWaitingHostKeyPrompt)
    {
        SessionManagerHostKeyTesting::AnsweringHostKeyProvider hostKeys{std::nullopt};
        manager_->setHostKeyProvider(&hostKeys);
        auto asked = hostKeys.firstAsked();

        auto connecting = startConnect(false);
        ASSERT_EQ(asked.wait_for(std::chrono::seconds{20}), std::future_status::ready);
        manager_->shutdown();

        const auto connected = awaitConnect(connecting);
        EXPECT_FALSE(connected.id);
        EXPECT_EQ(passwords_.asked, 0u);
    }
}
