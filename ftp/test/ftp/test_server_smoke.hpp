#pragma once

#ifndef _WIN32

#    include <ftp/support/server_fleet.hpp>
#    include <ftp/support/server_profile.hpp>
#    include <ftp/support/test_ftp_client.hpp>

#    include <fmt/format.h>
#    include <gtest/gtest.h>

#    include <string>

namespace FileTransferProtocol::Test
{
    /**
     * @brief That the fleet came up and the test setup can talk to it, the real conformance suites build on this.
     */
    class ServerSmoke : public ::testing::TestWithParam<std::string>
    {
      protected:
        void SetUp() override
        {
            if (auto const& reason = fleetUnavailable(); reason)
                GTEST_SKIP() << *reason;

            auto profile = serverProfile(GetParam());
            ASSERT_TRUE(profile.has_value()) << profile.error();
            profile_ = std::move(profile).value();
            auto endpoint = serverEndpoint(GetParam());
            ASSERT_TRUE(endpoint.has_value()) << endpoint.error();
            endpoint_ = std::move(endpoint).value();
        }

        void expectReply(std::string_view line, int code)
        {
            const auto reply = client_.command(line);
            ASSERT_TRUE(reply.has_value()) << line << ": " << reply.error();
            EXPECT_EQ(reply->code, code) << line << ": " << reply->text;
        }

        void login()
        {
            auto const& user = profile_.users.front();
            expectReply(fmt::format("USER {}", user.name), 331);
            expectReply(fmt::format("PASS {}", user.password), 230);
        }

        void connectAndReadGreeting()
        {
            const auto connected = client_.connect(endpoint_.address, profile_.plainPort);
            ASSERT_TRUE(connected.has_value()) << connected.error();
            const auto greeting = client_.readReply();
            ASSERT_TRUE(greeting.has_value()) << greeting.error();
            EXPECT_EQ(greeting->code, 220) << greeting->text;
        }

      protected:
        ServerProfile profile_{};
        ServerEndpoint endpoint_{};
        TestFtpClient client_{};
    };

    TEST_P(ServerSmoke, AnswersFeat)
    {
        connectAndReadGreeting();
        login();
        const auto features = client_.command("FEAT");
        ASSERT_TRUE(features.has_value()) << features.error();
        EXPECT_EQ(features->code, 211) << features->text;
        for (auto const& feature : profile_.expectations.advertised)
            EXPECT_NE(features->text.find(fmt::format("\n {}", feature)), std::string::npos) << feature;
    }

    TEST_P(ServerSmoke, AnswersFeatOverExplicitTls)
    {
        connectAndReadGreeting();
        expectReply("AUTH TLS", 234);
        const auto secured = client_.startTls();
        ASSERT_TRUE(secured.has_value()) << secured.error();
        login();
        expectReply("FEAT", 211);
    }

    TEST_P(ServerSmoke, GreetsOverImplicitTls)
    {
        const auto connected = client_.connect(endpoint_.address, profile_.implicitPort);
        ASSERT_TRUE(connected.has_value()) << connected.error();
        const auto secured = client_.startTls();
        ASSERT_TRUE(secured.has_value()) << secured.error();
        const auto greeting = client_.readReply();
        ASSERT_TRUE(greeting.has_value()) << greeting.error();
        EXPECT_EQ(greeting->code, 220) << greeting->text;
    }

    TEST_P(ServerSmoke, EveryVariantGreets)
    {
        for (auto const& [name, variant] : profile_.variants)
        {
            const auto endpoint = serverEndpoint(GetParam(), name);
            ASSERT_TRUE(endpoint.has_value()) << endpoint.error();
            TestFtpClient client{};
            const auto connected = client.connect(endpoint->address, profile_.plainPort);
            ASSERT_TRUE(connected.has_value()) << name << ": " << connected.error();
            const auto greeting = client.readReply();
            ASSERT_TRUE(greeting.has_value()) << name << ": " << greeting.error();
            EXPECT_EQ(greeting->code, 220) << name << ": " << greeting->text;
        }
    }

    TEST_P(ServerSmoke, AWorkspaceHoldsTheFixtures)
    {
        const auto workspace = prepareWorkspace(endpoint_);
        ASSERT_TRUE(workspace.has_value()) << workspace.error();

        connectAndReadGreeting();
        login();
        expectReply(fmt::format("CWD {}", *workspace), 250);
        expectReply("TYPE I", 200);
        const auto size = client_.command("SIZE chunk.bin");
        ASSERT_TRUE(size.has_value()) << size.error();
        EXPECT_EQ(size->text, "213 16384");
    }

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        ServerSmoke,
        ::testing::ValuesIn(selectedServers()),
        [](::testing::TestParamInfo<std::string> const& info)
        {
            return info.param;
        }
    );
}

#endif
