#pragma once

#include "common_fixture.hpp"
#include <ssh/host_key.hpp>
#include <ssh/session.hpp>
#include <paths/native.hpp>
#include <nui/utility/scope_exit.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"

static const char hostPublicKey[] = {
#embed "./test_ssh2_servers/key.openssh.public"
    ,
    '\0',
};

#pragma clang diagnostic pop

namespace SecureShell::Test
{
    /**
     * @brief Connects to the mock server, whose host key is the RSA key of the fixture, with a known hosts file of
     *        the test alone.
     */
    class HostKeyTests : public CommonFixture
    {
      protected:
        static constexpr std::string_view hostKeyFingerprint{"SHA256:YLyQlbOxr7jb8n53bx2ESk4whtSeG9qQgBfHxR/q5Go"};
        /** @brief Keys of no server. */
        static constexpr std::string_view anotherRsaKey{
            "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDfgwZlfQMDjI74RCLnv0+jlKB3rRfKyHHbvetboSv4GGnVJu1yZXcmrqwO8nTa2YdWjr1tCl2qBzPEzln+eU3Jj7Uhh5ebGzEnA0X/e8h5jlkPVBLQh40oC0wR5KcH5KQvxJZC9LC30CaqUc0l6TFxjvK/BlMdA9HbX+NgfaQwLfDqxaiDpytCwNxW9wYDz+DO25aHc1D/8+vbU3wfNxzmv3W7W9T1DNnb3pIB7l4fTZxfxgb+vNOVH4Clgu1//JuuOV76ddf5r4325m+DyXNLqd72sDMrM4iqetozl63QX08SCKlNr2N+SDacqJ29j2du3RDh808SuaqBLD05YaD9"
        };
        static constexpr std::string_view anotherEd25519Key{
            "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGA+4IwqE35ZvRFPzGpbwCdngq4Z86TtGF75oeyGiqWZ"
        };
        /** @brief A key of a type libssh 0.12 cannot read, as OpenSSH 10 adds them to the known hosts file. */
        static constexpr std::string_view unreadableKey{
            "ssh-mldsa44-ed25519 "
            "AAAAE3NzaC1tbGRzYTQ0LWVkMjU1MTkAAAVAAAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKiss"
            "LS4vMDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5fYGFiY2RlZmdoaWprbG1ub3BxcnN0"
            "dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6PkJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmqq6ytrq+wsbKztLW2t7i5uru8"
            "vb6/wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj5OXm5+jp6uvs7e7v8PHy8/T19vf4+fr7/P3+/wABAgME"
            "BQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj9AQUJDREVGR0hJSktM"
            "TU5PUFFSU1RVVldYWVpbXF1eX2BhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGCg4SFhoeIiYqLjI2Oj5CRkpOU"
            "lZaXmJmam5ydnp+goaKjpKWmp6ipqqusra6vsLGys7S1tre4ubq7vL2+v8DBwsPExcbHyMnKy8zNzs/Q0dLT1NXW19jZ2tvc"
            "3d7f4OHi4+Tl5ufo6err7O3u7/Dx8vP09fb3+Pn6+/z9/v8AAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyAhIiMk"
            "JSYnKCkqKywtLi8wMTIzNDU2Nzg5Ojs8PT4/QEFCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFlaW1xdXl9gYWJjZGVmZ2hpamts"
            "bW5vcHFyc3R1dnd4eXp7fH1+f4CBgoOEhYaHiImKi4yNjo+QkZKTlJWWl5iZmpucnZ6foKGio6SlpqeoqaqrrK2ur7CxsrO0"
            "tba3uLm6u7y9vr/AwcLDxMXGx8jJysvMzc7P0NHS09TV1tfY2drb3N3e3+Dh4uPk5ebn6Onq6+zt7u/w8fLz9PX29/j5+vv8"
            "/f7/AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+P0BBQkNE"
            "RUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5fYGFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouM"
            "jY6PkJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmqq6ytrq+wsbKztLW2t7i5uru8vb6/wMHCw8TFxsfIycrLzM3Oz9DR0tPU"
            "1dbX2Nna29zd3t/g4eLj5OXm5+jp6uvs7e7v8PHy8/T19vf4+fr7/P3+/wABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhsc"
            "HR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj9AQUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVpbXF1eX2BhYmNk"
            "ZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGCg4SFhoeIiYqLjI2Oj5CRkpOUlZaXmJmam5ydnp+goaKjpKWmp6ipqqus"
            "ra6vsLGys7S1tre4ubq7vL2+v8DBwsPExcbHyMnKy8zNzs/Q0dLT1NXW19jZ2tvc3d7f4OHi4+Tl5ufo6err7O3u7/Dx8vP0"
            "9fb3+Pn6+/z9/v8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
            "AAAA"
        };

        /**
         * @brief How a connect went, and what it asked on the way.
         */
        struct Attempt
        {
            std::expected<std::unique_ptr<Session>, std::string> session{};
            std::vector<HostKeyQuestion> questions{};
            bool passwordAsked{false};
        };

        void SetUp() override
        {
            CommonFixture::SetUp();
            auto [server, thread] = createSshServer();
            server_ = std::move(server);
            serverThread_ = std::move(thread);
            ASSERT_TRUE(server_);
        }

        void TearDown() override
        {
            if (server_)
                server_->command("exit");
            if (serverThread_.joinable())
                serverThread_.join();
        }

        std::filesystem::path knownHostsFile() const
        {
            return isolateDirectory_.path() / "known_hosts";
        }

        /**
         * @brief Makes @p key the one the known hosts file holds for the mock server.
         */
        void knowServerBy(std::string_view key)
        {
            std::ofstream file{knownHostsFile(), std::ios_base::binary};
            file << fmt::format("[127.0.0.1]:{} {}\n", server_->port, key);
        }

        /**
         * @brief Makes @p keys the ones the known hosts file holds for the mock server, in that order.
         */
        void knowServerBy(std::vector<std::string_view> const& keys)
        {
            std::ofstream file{knownHostsFile(), std::ios_base::binary};
            for (const auto key : keys)
                file << fmt::format("[127.0.0.1]:{} {}\n", server_->port, key);
        }

        /**
         * @brief The known hosts file as text: libssh ends its lines as the platform does.
         */
        std::string knownHosts() const
        {
            std::ifstream file{knownHostsFile()};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        /**
         * @param strict Nothing leaves it to the default.
         * @param answer What a host key prompt answers.
         */
        Attempt connect(std::optional<bool> strict, HostKeyAnswer answer)
        {
            auto options = getSessionOptions(server_->port);
            options.sshOptions->knownHostsFile = knownHostsFile();
            options.sshOptions->strictHostKeyCheck = strict;
            options.sshOptions->bypassConfig = true;
            return connect(std::move(options), answer);
        }

        Attempt connect(Persistence::SshSessionOptions options, HostKeyAnswer answer)
        {
            Attempt attempt{};
            attempt.session = makeSession(
                options,
                +[](char const*, char* buf, std::size_t length, int, int, void* userdata)
                {
                    *static_cast<bool*>(userdata) = true;
                    static constexpr std::string_view pw = "test";
                    std::strncpy(buf, pw.data(), std::min(pw.size(), length - 1));
                    return 0;
                },
                &attempt.passwordAsked,
                &attempt.passwordAsked,
                nullptr,
                [&attempt, answer](HostKeyQuestion const& question)
                {
                    attempt.questions.push_back(question);
                    return answer;
                },
                []() {}
            );
            return attempt;
        }

        /**
         * @brief The options of a connect to the mock server that takes the mock server as its jump host too.
         */
        Persistence::SshSessionOptions throughTheServerAsJumpHost()
        {
            auto options = getSessionOptions(server_->port);
            options.sshOptions->knownHostsFile = knownHostsFile();
            options.sshOptions->strictHostKeyCheck = false;
            options.sshOptions->bypassConfig = true;
            options.sshOptions->proxyJump = fmt::format("test@127.0.0.1:{}", server_->port);
            return options;
        }

        /**
         * @brief How verifyHostKey() decided, and what it asked on the way.
         */
        struct Verification
        {
            std::expected<void, std::string> verified{};
            std::vector<HostKeyQuestion> questions{};
        };

        struct SessionDeleter
        {
            void operator()(ssh_session session) const noexcept
            {
                ssh_disconnect(session);
                ssh_free(session);
            }
        };
        using OwnedSession = std::unique_ptr<ssh_session_struct, SessionDeleter>;

        /**
         * @brief A session of libssh alone that is connected to the mock server and has not looked at its host key,
         *        as libssh hands the session of a jump host to its verification.
         *
         * @param strictCheckingOfLibssh Off is how "StrictHostKeyChecking no" in an ssh config turns it.
         * @param globalKnownHostsFile Empty leaves the one of libssh.
         * @return Nothing, with a failure of the test, when there is no connection.
         */
        OwnedSession
        connectedSessionOfLibssh(bool strictCheckingOfLibssh, std::filesystem::path const& globalKnownHostsFile = {})
        {
            OwnedSession session{ssh_new()};
            const auto knownHosts = Paths::nativeToUtf8(knownHostsFile());
            const auto globalKnownHosts = Paths::nativeToUtf8(globalKnownHostsFile);
            const unsigned int port = server_->port;
            const int processConfig = 0;
            const int strictChecking = strictCheckingOfLibssh ? 1 : 0;

            if (!session || ssh_options_set(session.get(), SSH_OPTIONS_HOST, "127.0.0.1") != SSH_OK ||
                ssh_options_set(session.get(), SSH_OPTIONS_PORT, &port) != SSH_OK ||
                ssh_options_set(session.get(), SSH_OPTIONS_KNOWNHOSTS, knownHosts.c_str()) != SSH_OK ||
                (!globalKnownHosts.empty() &&
                 ssh_options_set(session.get(), SSH_OPTIONS_GLOBAL_KNOWNHOSTS, globalKnownHosts.c_str()) != SSH_OK) ||
                ssh_options_set(session.get(), SSH_OPTIONS_PROCESS_CONFIG, &processConfig) != SSH_OK ||
                ssh_options_set(session.get(), SSH_OPTIONS_STRICTHOSTKEYCHECK, &strictChecking) != SSH_OK ||
                ssh_connect(session.get()) != SSH_OK)
            {
                ADD_FAILURE() << "no connection to the mock server";
                return nullptr;
            }
            return session;
        }

        std::filesystem::path globalKnownHostsFile() const
        {
            return isolateDirectory_.path() / "global_known_hosts";
        }

        /**
         * @brief Makes @p key the one the global known hosts file holds for the mock server.
         */
        void knowServerGloballyBy(std::string_view key)
        {
            std::ofstream file{globalKnownHostsFile(), std::ios_base::binary};
            file << fmt::format("[127.0.0.1]:{} {}\n", server_->port, key);
        }

        /**
         * @brief Verifies the host key of the mock server with the global known hosts file of the test.
         */
        Verification verifyWithGlobalKnownHostsFile(HostKeyAnswer answer)
        {
            return verifyOn(connectedSessionOfLibssh(true, globalKnownHostsFile()), false, answer);
        }

        /**
         * @brief Verifies the host key of the mock server on a session whose StrictHostKeyChecking of libssh is off.
         */
        Verification verifyWithStrictCheckingOfLibsshOff(bool strict, HostKeyAnswer answer)
        {
            return verifyOn(connectedSessionOfLibssh(false), strict, answer);
        }

        Verification verifyOn(OwnedSession const& session, bool strict, HostKeyAnswer answer)
        {
            Verification verification{};
            if (!session)
            {
                verification.verified = std::unexpected{std::string{"no connection to the mock server"}};
                return verification;
            }

            verification.verified = verifyHostKey(
                session.get(),
                strict,
                [&verification, answer](HostKeyQuestion const& question)
                {
                    verification.questions.push_back(question);
                    return answer;
                }
            );
            return verification;
        }

        std::shared_ptr<NodeProcessResult> server_{};
        std::thread serverThread_{};
    };

    TEST_F(HostKeyTests, AKnownHostKeyConnectsWithoutAQuestion)
    {
        knowServerBy(hostPublicKey);

        const auto attempt = connect(true, HostKeyAnswer::Refused);

        ASSERT_TRUE(attempt.session.has_value()) << attempt.session.error();
        EXPECT_TRUE(attempt.questions.empty());
        EXPECT_TRUE(attempt.passwordAsked);
    }

    TEST_F(HostKeyTests, AKnownHostKeyConnectsBesideAKeyOfATypeLibsshCannotRead)
    {
        for (const auto unreadableFirst : {true, false})
        {
            if (unreadableFirst)
                knowServerBy(std::vector<std::string_view>{unreadableKey, hostPublicKey});
            else
                knowServerBy(std::vector<std::string_view>{hostPublicKey, unreadableKey});
            const auto before = knownHosts();

            const auto attempt = connect(true, HostKeyAnswer::Refused);

            ASSERT_TRUE(attempt.session.has_value()) << attempt.session.error();
            EXPECT_TRUE(attempt.questions.empty());
            EXPECT_EQ(knownHosts(), before);
        }
    }

    TEST_F(HostKeyTests, AChangedHostKeyIsRefusedBesideAKeyOfATypeLibsshCannotRead)
    {
        knowServerBy(std::vector<std::string_view>{anotherRsaKey, unreadableKey});

        const auto attempt = connect(false, HostKeyAnswer::Always);

        ASSERT_FALSE(attempt.session.has_value());
        EXPECT_FALSE(attempt.passwordAsked);
        ASSERT_EQ(attempt.questions.size(), 1u);
        EXPECT_EQ(attempt.questions.front().problem, HostKeyProblem::Changed);
    }

    TEST_F(HostKeyTests, AServerKnownOnlyByAKeyLibsshCannotReadIsAskedAbout)
    {
        knowServerBy(std::vector<std::string_view>{unreadableKey});

        const auto attempt = connect(false, HostKeyAnswer::Refused);

        ASSERT_FALSE(attempt.session.has_value());
        ASSERT_EQ(attempt.questions.size(), 1u);
        EXPECT_EQ(attempt.questions.front().problem, HostKeyProblem::Unknown);
    }

    TEST_F(HostKeyTests, AChangedHostKeyIsRefusedBeforeThePasswordIsAskedWhateverIsAnswered)
    {
        knowServerBy(anotherRsaKey);

        for (const auto strict : {true, false})
        {
            const auto attempt = connect(strict, HostKeyAnswer::Always);

            ASSERT_FALSE(attempt.session.has_value()) << "strict: " << strict;
            EXPECT_FALSE(attempt.passwordAsked);
            // Put to the source so that the user is warned.
            ASSERT_EQ(attempt.questions.size(), 1u);
            EXPECT_EQ(attempt.questions.front().problem, HostKeyProblem::Changed);
            EXPECT_EQ(attempt.questions.front().fingerprint, hostKeyFingerprint);
            EXPECT_EQ(attempt.questions.front().knownHostsFile, Paths::nativeToUtf8(knownHostsFile()));
            auto const& error = attempt.session.error();
            EXPECT_NE(error.find("may be intercepting"), std::string::npos) << error;
            EXPECT_NE(error.find(hostKeyFingerprint), std::string::npos) << error;
            EXPECT_NE(error.find(Paths::nativeToUtf8(knownHostsFile())), std::string::npos) << error;
        }
        EXPECT_EQ(knownHosts(), fmt::format("[127.0.0.1]:{} {}\n", server_->port, anotherRsaKey));
    }

    TEST_F(HostKeyTests, AHostKeyOfAnotherTypeThanTheKnownOneIsRefused)
    {
        knowServerBy(anotherEd25519Key);

        const auto attempt = connect(false, HostKeyAnswer::Always);

        ASSERT_FALSE(attempt.session.has_value());
        EXPECT_FALSE(attempt.passwordAsked);
        EXPECT_NE(attempt.session.error().find("another type"), std::string::npos) << attempt.session.error();
        ASSERT_EQ(attempt.questions.size(), 1u);
        EXPECT_EQ(attempt.questions.front().problem, HostKeyProblem::OtherType);
        EXPECT_EQ(knownHosts(), fmt::format("[127.0.0.1]:{} {}\n", server_->port, anotherEd25519Key));
    }

    TEST_F(HostKeyTests, AChangedHostKeyIsRefusedWithoutSomeoneToWarn)
    {
        knowServerBy(anotherRsaKey);
        auto options = getSessionOptions(server_->port);
        options.sshOptions->knownHostsFile = knownHostsFile();
        options.sshOptions->bypassConfig = true;
        bool passwordAsked = false;

        const auto session = makeSession(
            options,
            +[](char const*, char*, std::size_t, int, int, void* userdata)
            {
                *static_cast<bool*>(userdata) = true;
                return -1;
            },
            &passwordAsked,
            &passwordAsked,
            nullptr,
            {},
            []() {}
        );

        ASSERT_FALSE(session.has_value());
        EXPECT_FALSE(passwordAsked);
        EXPECT_NE(session.error().find("may be intercepting"), std::string::npos) << session.error();
    }

    TEST_F(HostKeyTests, AChangedHostKeyIsRefusedWhenTheSshConfigTurnsStrictCheckingOff)
    {
        for (const auto key : {anotherRsaKey, anotherEd25519Key})
        {
            knowServerBy(key);
            for (const auto strict : {true, false})
            {
                const auto verification = verifyWithStrictCheckingOfLibsshOff(strict, HostKeyAnswer::Always);

                EXPECT_FALSE(verification.verified.has_value()) << "strict: " << strict << ", known: " << key;
                EXPECT_EQ(verification.questions.size(), 1u);
            }
            EXPECT_EQ(knownHosts(), fmt::format("[127.0.0.1]:{} {}\n", server_->port, key));
        }
    }

    TEST_F(HostKeyTests, AnUnknownHostKeyIsNotTrustedUnaskedWhenTheSshConfigTurnsStrictCheckingOff)
    {
        for (const auto strict : {true, false})
        {
            const auto verification = verifyWithStrictCheckingOfLibsshOff(strict, HostKeyAnswer::Refused);

            EXPECT_FALSE(verification.verified.has_value()) << "strict: " << strict;
            EXPECT_EQ(verification.questions.size(), strict ? 0u : 1u);
        }
    }

    TEST_F(HostKeyTests, AChangedHostKeyComesWithTheCommandThatRemovesTheOldKeyOnLinux)
    {
        for (const auto known : {anotherRsaKey, anotherEd25519Key})
        {
            knowServerBy(known);

            const auto attempt = connect(false, HostKeyAnswer::Always);

            ASSERT_EQ(attempt.questions.size(), 1u);
#ifdef __linux__
            EXPECT_EQ(
                attempt.questions.front().removalCommand,
                fmt::format(
                    "ssh-keygen -f '{}' -R '[127.0.0.1]:{}'", Paths::nativeToUtf8(knownHostsFile()), server_->port
                )
            );
#else
            EXPECT_TRUE(attempt.questions.front().removalCommand.empty());
#endif
        }
    }

    TEST_F(HostKeyTests, AnUnknownHostKeyComesWithoutACommandToRemoveAKey)
    {
        const auto attempt = connect(false, HostKeyAnswer::Refused);

        ASSERT_EQ(attempt.questions.size(), 1u);
        EXPECT_FALSE(attempt.questions.front().jumpHost);
        EXPECT_TRUE(attempt.questions.front().removalCommand.empty());
    }

    TEST_F(HostKeyTests, AChangedHostKeyNamesTheGlobalKnownHostsFileThatHoldsTheOtherKey)
    {
        knowServerGloballyBy(anotherRsaKey);

        const auto verification = verifyWithGlobalKnownHostsFile(HostKeyAnswer::Always);

        ASSERT_FALSE(verification.verified.has_value());
        ASSERT_EQ(verification.questions.size(), 1u);
        const auto globalFile = Paths::nativeToUtf8(globalKnownHostsFile());
        EXPECT_EQ(verification.questions.front().problem, HostKeyProblem::Changed);
        EXPECT_EQ(verification.questions.front().knownHostsFile, globalFile);
        EXPECT_NE(verification.verified.error().find(globalFile), std::string::npos) << verification.verified.error();
    }

    TEST_F(HostKeyTests, AChangedHostKeyNamesTheKnownHostsFileOfTheUserWhenBothHoldAnotherKey)
    {
        knowServerBy(anotherRsaKey);
        knowServerGloballyBy(anotherRsaKey);

        const auto verification = verifyWithGlobalKnownHostsFile(HostKeyAnswer::Always);

        ASSERT_FALSE(verification.verified.has_value());
        ASSERT_EQ(verification.questions.size(), 1u);
        EXPECT_EQ(verification.questions.front().knownHostsFile, Paths::nativeToUtf8(knownHostsFile()));
    }

    TEST_F(HostKeyTests, AJumpHostKeyTheKnownHostsFileHoldsPasses)
    {
        knowServerBy(hostPublicKey);
        const auto jumpSession = connectedSessionOfLibssh(true);
        ASSERT_TRUE(jumpSession);

        const auto verified = verifyJumpHostKey(jumpSession.get());

        EXPECT_TRUE(verified.has_value()) << verified.error().reason;
    }

    TEST_F(HostKeyTests, AnUnknownJumpHostKeyIsRefusedWhateverTheSshConfigSays)
    {
        for (const auto strictCheckingOfLibssh : {true, false})
        {
            const auto jumpSession = connectedSessionOfLibssh(strictCheckingOfLibssh);
            ASSERT_TRUE(jumpSession);

            const auto verified = verifyJumpHostKey(jumpSession.get());

            ASSERT_FALSE(verified.has_value()) << "strict checking of libssh: " << strictCheckingOfLibssh;
            auto const& refusal = verified.error();
            EXPECT_NE(refusal.reason.find("jump host 127.0.0.1"), std::string::npos) << refusal.reason;
            EXPECT_NE(refusal.reason.find(hostKeyFingerprint), std::string::npos) << refusal.reason;
            EXPECT_EQ(refusal.question.problem, HostKeyProblem::Unknown);
            EXPECT_TRUE(refusal.question.jumpHost);
            EXPECT_EQ(refusal.question.port, server_->port);
            EXPECT_EQ(refusal.question.fingerprint, hostKeyFingerprint);
        }
        EXPECT_FALSE(std::filesystem::exists(knownHostsFile()));
    }

    TEST_F(HostKeyTests, AChangedJumpHostKeyIsRefusedWhateverTheSshConfigSays)
    {
        knowServerBy(anotherRsaKey);
        for (const auto strictCheckingOfLibssh : {true, false})
        {
            const auto jumpSession = connectedSessionOfLibssh(strictCheckingOfLibssh);
            ASSERT_TRUE(jumpSession);

            const auto verified = verifyJumpHostKey(jumpSession.get());

            ASSERT_FALSE(verified.has_value()) << "strict checking of libssh: " << strictCheckingOfLibssh;
            auto const& refusal = verified.error();
            EXPECT_NE(refusal.reason.find("may be intercepting"), std::string::npos) << refusal.reason;
            EXPECT_EQ(refusal.question.problem, HostKeyProblem::Changed);
            EXPECT_EQ(refusal.question.knownHostsFile, Paths::nativeToUtf8(knownHostsFile()));
        }
    }

    TEST_F(HostKeyTests, AJumpHostKeyOfAnotherTypeThanTheKnownOneIsRefused)
    {
        knowServerBy(anotherEd25519Key);
        const auto jumpSession = connectedSessionOfLibssh(false);
        ASSERT_TRUE(jumpSession);

        const auto verified = verifyJumpHostKey(jumpSession.get());

        ASSERT_FALSE(verified.has_value());
        EXPECT_EQ(verified.error().question.problem, HostKeyProblem::OtherType);
    }

#ifndef _WIN32
    // libssh has no ProxyJump on Windows. The mock server is the jump host too.
    TEST_F(HostKeyTests, AConnectThroughAnUnknownJumpHostFailsWithItsKeyBeforeAnythingIsAsked)
    {
        const auto attempt = connect(throughTheServerAsJumpHost(), HostKeyAnswer::Always);

        ASSERT_FALSE(attempt.session.has_value());
        EXPECT_FALSE(attempt.passwordAsked);
        EXPECT_TRUE(attempt.questions.empty());
        auto const& error = attempt.session.error();
        EXPECT_NE(error.find(fmt::format("jump host 127.0.0.1:{}", server_->port)), std::string::npos) << error;
        EXPECT_NE(error.find(hostKeyFingerprint), std::string::npos) << error;
        EXPECT_FALSE(std::filesystem::exists(knownHostsFile()));
    }

    TEST_F(HostKeyTests, AJumpHostIsLookedUpInTheKnownHostsFileOfTheSession)
    {
        // The known hosts files of the user do not hold the port of a test, so only the file of the session can
        // tell that the key changed.
        knowServerBy(anotherRsaKey);

        const auto attempt = connect(throughTheServerAsJumpHost(), HostKeyAnswer::Always);

        ASSERT_FALSE(attempt.session.has_value());
        EXPECT_FALSE(attempt.passwordAsked);
        ASSERT_EQ(attempt.questions.size(), 1u) << "the warning";
        EXPECT_EQ(attempt.questions.front().problem, HostKeyProblem::Changed);
        EXPECT_TRUE(attempt.questions.front().jumpHost) << "the warning has to say whose key changed";
        EXPECT_EQ(attempt.questions.front().knownHostsFile, Paths::nativeToUtf8(knownHostsFile()));
        EXPECT_NE(attempt.session.error().find("may be intercepting"), std::string::npos) << attempt.session.error();
    }

    TEST_F(HostKeyTests, AJumpHostIsRefusedWhenNoConnectIsGoingOn)
    {
        knowServerBy(hostPublicKey);
        const OwnedSession session{ssh_new()};
        ASSERT_TRUE(session);
        const auto knownHosts = Paths::nativeToUtf8(knownHostsFile());
        const auto jump = fmt::format("test@127.0.0.1:{}", server_->port);
        const unsigned int port = server_->port;
        const int processConfig = 0;
        ASSERT_EQ(ssh_options_set(session.get(), SSH_OPTIONS_HOST, "127.0.0.1"), SSH_OK);
        ASSERT_EQ(ssh_options_set(session.get(), SSH_OPTIONS_PORT, &port), SSH_OK);
        ASSERT_EQ(ssh_options_set(session.get(), SSH_OPTIONS_KNOWNHOSTS, knownHosts.c_str()), SSH_OK);
        ASSERT_EQ(ssh_options_set(session.get(), SSH_OPTIONS_PROCESS_CONFIG, &processConfig), SSH_OK);
        ASSERT_EQ(ssh_options_set(session.get(), SSH_OPTIONS_PROXYJUMP, jump.c_str()), SSH_OK);
        {
            const auto ended = JumpHostVerification::begin(session.get(), {});
            ASSERT_TRUE(ended.has_value());
        }

        EXPECT_NE(ssh_connect(session.get()), SSH_OK);
    }
#endif

    TEST_F(HostKeyTests, ASessionTakesTheVerificationOfItsJumpHosts)
    {
        const OwnedSession session{ssh_new()};
        ASSERT_TRUE(session);

        auto verification = JumpHostVerification::begin(session.get(), {});

        ASSERT_TRUE(verification.has_value());
        EXPECT_FALSE(verification->takeRefusal().has_value());
    }

    TEST_F(HostKeyTests, AnUnknownHostKeyIsRefusedWithoutAQuestionWhenStrict)
    {
        // Both with a known hosts file that does not exist and one that names other servers.
        for (const auto withFile : {false, true})
        {
            std::filesystem::remove(knownHostsFile());
            if (withFile)
                std::ofstream{knownHostsFile()} << "[127.0.0.1]:1 " << hostPublicKey << "\n";
            const auto before = knownHosts();

            const auto attempt = connect(true, HostKeyAnswer::Always);

            ASSERT_FALSE(attempt.session.has_value());
            EXPECT_TRUE(attempt.questions.empty());
            EXPECT_FALSE(attempt.passwordAsked);
            auto const& error = attempt.session.error();
            EXPECT_NE(error.find("Strict Host Key Verification"), std::string::npos) << error;
            EXPECT_NE(error.find(hostKeyFingerprint), std::string::npos) << error;
            EXPECT_EQ(knownHosts(), before);
        }
    }

    TEST_F(HostKeyTests, AnUnknownHostKeyIsAskedAboutWhenTheOptionsDoNotSay)
    {
        const auto attempt = connect(std::nullopt, HostKeyAnswer::Refused);

        ASSERT_FALSE(attempt.session.has_value());
        EXPECT_EQ(attempt.questions.size(), 1u);
        EXPECT_FALSE(attempt.passwordAsked);
    }

    TEST_F(HostKeyTests, AnUnknownHostKeyIsAskedAboutWhenNotStrict)
    {
        const auto attempt = connect(false, HostKeyAnswer::Refused);

        ASSERT_EQ(attempt.questions.size(), 1u);
        auto const& question = attempt.questions.front();
        EXPECT_EQ(question.problem, HostKeyProblem::Unknown);
        EXPECT_EQ(question.host, "127.0.0.1");
        EXPECT_EQ(question.port, server_->port);
        EXPECT_EQ(question.keyType, "ssh-rsa");
        EXPECT_EQ(question.fingerprint, hostKeyFingerprint);
    }

    TEST_F(HostKeyTests, ARefusedHostKeyEndsTheConnectBeforeThePasswordIsAsked)
    {
        const auto attempt = connect(false, HostKeyAnswer::Refused);

        ASSERT_FALSE(attempt.session.has_value());
        EXPECT_FALSE(attempt.passwordAsked);
        EXPECT_NE(attempt.session.error().find("was not trusted"), std::string::npos) << attempt.session.error();
        EXPECT_FALSE(std::filesystem::exists(knownHostsFile()));
    }

    TEST_F(HostKeyTests, WithoutSomeoneToAskAnUnknownHostKeyIsRefused)
    {
        auto options = getSessionOptions(server_->port);
        options.sshOptions->knownHostsFile = knownHostsFile();
        options.sshOptions->strictHostKeyCheck = false;
        options.sshOptions->bypassConfig = true;
        bool passwordAsked = false;

        const auto session = makeSession(
            options,
            +[](char const*, char*, std::size_t, int, int, void* userdata)
            {
                *static_cast<bool*>(userdata) = true;
                return -1;
            },
            &passwordAsked,
            &passwordAsked,
            nullptr,
            {},
            []() {}
        );

        ASSERT_FALSE(session.has_value());
        EXPECT_FALSE(passwordAsked);
    }

    TEST_F(HostKeyTests, AHostKeyTrustedOnceConnectsAndStaysUnknown)
    {
        const auto attempt = connect(false, HostKeyAnswer::Once);

        ASSERT_TRUE(attempt.session.has_value()) << attempt.session.error();
        EXPECT_TRUE(attempt.passwordAsked);
        EXPECT_FALSE(std::filesystem::exists(knownHostsFile()));
    }

    TEST_F(HostKeyTests, AHostKeyTrustedAlwaysConnectsWhenTheKnownHostsFileCannotBeWritten)
    {
        // No directory can be made where a file is.
        const auto blocker = isolateDirectory_.path() / "blocker";
        std::ofstream{blocker} << "a file";
        auto options = getSessionOptions(server_->port);
        options.sshOptions->knownHostsFile = blocker / "known_hosts";
        options.sshOptions->strictHostKeyCheck = false;
        options.sshOptions->bypassConfig = true;

        const auto attempt = connect(std::move(options), HostKeyAnswer::Always);

        ASSERT_TRUE(attempt.session.has_value()) << attempt.session.error();
        EXPECT_EQ(attempt.questions.size(), 1u);
        EXPECT_TRUE(std::filesystem::is_regular_file(blocker));
    }

    TEST_F(HostKeyTests, AHostKeyTrustedAlwaysIsKnownFromThenOn)
    {
        {
            const auto attempt = connect(false, HostKeyAnswer::Always);
            ASSERT_TRUE(attempt.session.has_value()) << attempt.session.error();
            EXPECT_EQ(attempt.questions.size(), 1u);
        }
        EXPECT_EQ(knownHosts(), fmt::format("[127.0.0.1]:{} {}\n", server_->port, hostPublicKey));

        const auto strictAttempt = connect(true, HostKeyAnswer::Refused);

        ASSERT_TRUE(strictAttempt.session.has_value()) << strictAttempt.session.error();
        EXPECT_TRUE(strictAttempt.questions.empty());
    }
}
