#pragma once

#include "common_fixture.hpp"
#include <ssh/session.hpp>
#include <nui/utility/scope_exit.hpp>

#include <gtest/gtest.h>

#include <filesystem>

extern std::filesystem::path programDirectory;

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace SecureShell::Test
{
    class SshSessionTests : public CommonFixture
    {
        void TearDown() override
        {
            if (std::filesystem::exists(programDirectory / "temp" / "log.txt"))
            {
                std::filesystem::rename(
                    programDirectory / "temp" / "log.txt",
                    programDirectory / "temp" /
                        ("log_"s + ::testing::UnitTest::GetInstance()->current_test_info()->test_case_name() + "_"s +
                            ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".txt")
                );
            }
        }
    };

    TEST_F(SshSessionTests, CanCreateSshSession)
    {
        SecureShell::Session client{[]() {}};
    }

    TEST_F(SshSessionTests, CanStartAndStopSession)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();
        EXPECT_TRUE(session->isRunning());
        session->stop();
        EXPECT_FALSE(session->isRunning());
    }

    TEST_F(SshSessionTests, CanConnectToSshServer)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel.has_value());
        auto channel = expectedChannel.value().lock();

        std::promise<void> awaiter{};
        channelStartReading(channel, awaiter);

        channel->write("exit");
        channel->write("\r");
        ASSERT_EQ(awaiter.get_future().wait_for(5s), std::future_status::ready);

        channel->close();

        EXPECT_EQ(result->killed, false);
        EXPECT_EQ(result->code, 0);
    }

    TEST_F(SshSessionTests, CanConnectToSshServerAndDestructWithoutCrash)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto sessionScope = [this, &result]()
        {
            auto expectedSession = makePasswordTestSession(result->port);

            ASSERT_TRUE(expectedSession.has_value());
            auto session = std::move(expectedSession).value();
            session->start();
        };

        ASSERT_NO_FATAL_FAILURE(sessionScope());
    }

    TEST_F(SshSessionTests, CanConnectToSshServerCreateChannelAndDestructWithoutCrash)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto sessionScope = [this, &result]()
        {
            auto expectedSession = makePasswordTestSession(result->port);

            ASSERT_TRUE(expectedSession.has_value());
            auto session = std::move(expectedSession).value();
            session->start();

            auto expectedChannel = session->createPtyChannel({}).get();
            ASSERT_TRUE(expectedChannel.has_value());
            auto channel = expectedChannel.value().lock();
        };

        ASSERT_NO_FATAL_FAILURE(sessionScope());
    }

    TEST_F(SshSessionTests, CanOpenMultipleChannels)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel1 = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel1.has_value());
        auto channel1 = expectedChannel1.value().lock();

        auto expectedChannel2 = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel2.has_value()) << expectedChannel2.error();
        auto channel2 = expectedChannel2.value().lock();

        std::string channel1Output{};
        std::string channel2Output{};

        std::promise<void> awaiter1{};
        channelStartReading(
            channel1,
            [&channel1Output, &awaiter1](std::string const& output)
            {
                channel1Output += output;
                if (channel1Output.find("bashrc") != std::string::npos)
                    awaiter1.set_value();
            }
        );

        std::promise<void> awaiter2{};
        channelStartReading(
            channel2,
            [&channel2Output, &awaiter2](std::string const& output)
            {
                channel2Output += output;
                if (channel2Output.find("bashrc") != std::string::npos)
                    awaiter2.set_value();
            }
        );

        channel1->write("ls -lah");
        channel1->write("\r");

        channel2->write("ls -lah");
        channel2->write("\r");

        ASSERT_EQ(awaiter1.get_future().wait_for(5s), std::future_status::ready);
        ASSERT_EQ(awaiter2.get_future().wait_for(5s), std::future_status::ready);

        channel1->close();
        channel2->close();

        session->stop();

        EXPECT_GT(channel1Output.size(), 0);
        EXPECT_GT(channel2Output.size(), 0);

        EXPECT_NE(channel1Output, channel2Output);

        EXPECT_EQ(result->killed, false);
        EXPECT_EQ(result->code, 0);
    }

    TEST_F(SshSessionTests, ConnectFailsWithWrongCredentials)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto session = makeSession(
            getSessionOptions(result->port),
            +[](char const*, char* buf, std::size_t length, int, int, void*)
            {
                static constexpr std::string_view pw = "wrong";
                std::strncpy(buf, pw.data(), std::min(pw.size(), length - 1));
                return 0;
            },
            nullptr,
            nullptr,
            nullptr,
            []() {}
        );

        EXPECT_FALSE(session.has_value());
    }

    TEST_F(SshSessionTests, ConnectFailsWithWrongUser)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto session = makeSession(
            getSessionOptions(result->port, "wrong"),
            +[](char const*, char* buf, std::size_t length, int, int, void*)
            {
                static constexpr std::string_view pw = "test";
                std::strncpy(buf, pw.data(), std::min(pw.size(), length - 1));
                return 0;
            },
            nullptr,
            nullptr,
            nullptr,
            []() {}
        );

        EXPECT_FALSE(session.has_value());
    }

    TEST_F(SshSessionTests, ConnectFailsWithWrongPortWithinTheTimeout)
    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        auto session = makeSession(
            getSessionOptions(0),
            +[](char const*, char* buf, std::size_t length, int, int, void*)
            {
                static constexpr std::string_view pw = "test";
                std::strncpy(buf, pw.data(), std::min(pw.size(), length - 1));
                return 0;
            },
            nullptr,
            nullptr,
            nullptr,
            []() {}
        );
        auto diff = std::chrono::steady_clock::now() - start;
        // 1s leniency
        EXPECT_LT(diff, (connectTimeout + 1s));
        EXPECT_FALSE(session.has_value());
    }

    TEST_F(SshSessionTests, ConnectFailsWithWrongAddressWithinTheTimeout)
    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        auto session = makeSession(
            getSessionOptions(22, "test", "0100::"),
            +[](char const*, char* buf, std::size_t length, int, int, void*)
            {
                static constexpr std::string_view pw = "test";
                std::strncpy(buf, pw.data(), std::min(pw.size(), length - 1));
                return 0;
            },
            nullptr,
            nullptr,
            nullptr,
            []() {}
        );
        auto diff = std::chrono::steady_clock::now() - start;
        // 1s leniency
        EXPECT_LT(diff, (connectTimeout + 1s));
        EXPECT_FALSE(session.has_value());
    }

    // Determined by js test program
    TEST_F(SshSessionTests, RequestingInvalidTerminalPtyFails)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                // result->command("exit");
                result->terminate();
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel = session->createPtyChannel({.terminalType = "invalid"}).get();
        ASSERT_FALSE(expectedChannel.has_value());
    }

    TEST_F(SshSessionTests, CanResizePtyChannel)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel.has_value());
        auto channel = expectedChannel.value().lock();

        auto resizeFut = channel->resizePty(80, 24);
        ASSERT_EQ(resizeFut.wait_for(5s), std::future_status::ready);
        EXPECT_EQ(resizeFut.get(), 0);

        channel->close();

        EXPECT_EQ(result->killed, false);
        EXPECT_EQ(result->code, 0);
    }

    TEST_F(SshSessionTests, ClosingOneChannelDoesNotAffectTheOther)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel1 = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel1.has_value());
        auto channel1 = expectedChannel1.value().lock();

        auto expectedChannel2 = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel2.has_value());
        auto channel2 = expectedChannel2.value().lock();

        std::string channel2Output{};

        channelStartReading(channel1);
        channel1->close();

        std::promise<void> awaiter2{};
        channelStartReading(
            channel2,
            [&channel2Output, &awaiter2](std::string const& output)
            {
                channel2Output += output;
                if (channel2Output.find("bashrc") != std::string::npos)
                    awaiter2.set_value();
            }
        );

        channel2->write("ls -lah");
        channel2->write("\r");

        ASSERT_EQ(awaiter2.get_future().wait_for(5s), std::future_status::ready);

        channel2->close();
        session->stop();

        EXPECT_GT(channel2Output.size(), 0);

        EXPECT_EQ(result->killed, false);
        EXPECT_EQ(result->code, 0);
    }

    TEST_F(SshSessionTests, CanRunOutOfScopeWhileReadingWithoutIssue)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto sessionScope = [this, &result]()
        {
            auto expectedSession = makePasswordTestSession(result->port);

            ASSERT_TRUE(expectedSession.has_value());
            auto session = std::move(expectedSession).value();
            session->start();

            auto expectedChannel = session->createPtyChannel({}).get();
            ASSERT_TRUE(expectedChannel.has_value());
            auto channel = expectedChannel.value().lock();

            std::promise<void> awaiter{};
            channelStartReading(channel, awaiter);

            channel->write("ls -lah");
            channel->write("\r");
        };

        ASSERT_NO_FATAL_FAILURE(sessionScope());
    }

    TEST_F(SshSessionTests, CannotStartReadingOnClosedChannel)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel.has_value());
        auto channel = expectedChannel.value().lock();

        channel->close();

        std::promise<void> awaiter{};
        EXPECT_THROW(channelStartReading(channel, awaiter), std::runtime_error);
    }

    TEST_F(SshSessionTests, CannotWriteAfterChannelClose)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel.has_value());
        auto channel = expectedChannel.value().lock();

        channel->close();

        EXPECT_EQ(channel->write("ls -lah"), false);
    }

    TEST_F(SshSessionTests, CannotResizePtyAfterChannelClose)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel.has_value());
        auto channel = expectedChannel.value().lock();

        channel->close();

        auto resizeFut = channel->resizePty(80, 24);
        ASSERT_EQ(resizeFut.wait_for(5s), std::future_status::ready);
        EXPECT_EQ(resizeFut.get(), SSH_ERROR);
    }

    TEST_F(SshSessionTests, CannotCloseChannelTwice)
    {
        auto [result, processThread] = createSshServer();
        ASSERT_TRUE(result);
        auto joiner = Nui::ScopeExit{[&]() noexcept
            {
                result->command("exit");
                if (processThread.joinable())
                    processThread.join();
            }};

        auto expectedSession = makePasswordTestSession(result->port);

        ASSERT_TRUE(expectedSession.has_value());
        auto session = std::move(expectedSession).value();
        session->start();

        auto expectedChannel = session->createPtyChannel({}).get();
        ASSERT_TRUE(expectedChannel.has_value());
        auto channel = expectedChannel.value().lock();

        channel->close();
        EXPECT_FALSE(channel->close());
    }
}
