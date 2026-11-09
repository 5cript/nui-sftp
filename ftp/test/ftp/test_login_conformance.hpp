#pragma once

#ifndef _WIN32

#    include <ftp/client/control_connection.hpp>
#    include <ftp/client/login.hpp>
#    include <ftp/client/tls.hpp>
#    include <ftp/support/server_fleet.hpp>
#    include <ftp/support/server_profile.hpp>

#    include <boost/asio/co_spawn.hpp>
#    include <boost/asio/ip/address.hpp>
#    include <boost/asio/thread_pool.hpp>
#    include <boost/asio/use_future.hpp>
#    include <fmt/format.h>
#    include <gtest/gtest.h>

#    include <chrono>
#    include <future>
#    include <memory>
#    include <stdexcept>
#    include <string>
#    include <tuple>
#    include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief Logs in to every selected server, plain and with both kinds of TLS. The servers present self-signed
     *        certificates, so a TLS login goes the way of a trust prompt: refused first, then pinned.
     */
    class LoginConformance : public ::testing::TestWithParam<std::tuple<std::string, TlsMode>>
    {
      protected:
        void SetUp() override
        {
            if (auto const& reason = fleetUnavailable(); reason)
                GTEST_SKIP() << *reason;
            auto profile = serverProfile(server());
            ASSERT_TRUE(profile.has_value()) << profile.error();
            profile_ = std::move(profile).value();
            auto endpoint = serverEndpoint(server());
            ASSERT_TRUE(endpoint.has_value()) << endpoint.error();
            endpoint_ = std::move(endpoint).value();
        }

        void TearDown() override
        {
            for (auto const& connection : connections_)
                connection->close();
        }

        std::string const& server() const
        {
            return std::get<0>(GetParam());
        }

        TlsMode mode() const
        {
            return std::get<1>(GetParam());
        }

        std::uint16_t port() const
        {
            if (mode() == TlsMode::Implicit)
                return profile_.implicitPort;
            if (mode() == TlsMode::Explicit)
                return profile_.explicitPort;
            return profile_.plainPort;
        }

        LoginOptions options(std::optional<Sha256Digest> pin = std::nullopt) const
        {
            auto const& user = profile_.users.front();
            LoginOptions options{
                .tlsMode = mode(),
                .credentials = {.user = user.name, .password = user.password},
            };
            if (mode() == TlsMode::None)
                return options;
            auto context = TlsContext::create({});
            if (!context)
                throw std::runtime_error{context.error().toString()};
            options.tls = TlsConnectionSettings{
                .context = std::move(context).value(),
                .serverName = endpoint_.address,
                .pinnedPublicKey = pin,
            };
            return options;
        }

        std::shared_ptr<ControlConnection> makeConnection()
        {
            auto connection = ControlConnection::create(pool_.get_executor());
            connections_.push_back(connection);
            return connection;
        }

        std::expected<LoginResult, RemoteIo::RemoteError>
        login(std::shared_ptr<ControlConnection> const& connection, LoginOptions loginOptions)
        {
            const std::vector<boost::asio::ip::tcp::endpoint> endpoints{
                {boost::asio::ip::make_address(endpoint_.address), port()}
            };
            auto future = boost::asio::co_spawn(
                connection->strand(), logIn(connection, endpoints, std::move(loginOptions)), boost::asio::use_future
            );
            if (future.wait_for(std::chrono::seconds{30}) != std::future_status::ready)
                throw std::runtime_error{"The login did not end"};
            return future.get();
        }

        template <typename FunctionT>
        auto onStrand(std::shared_ptr<ControlConnection> const& connection, FunctionT function)
        {
            auto future = boost::asio::co_spawn(connection->strand(), std::move(function), boost::asio::use_future);
            if (future.wait_for(std::chrono::seconds{30}) != std::future_status::ready)
                throw std::runtime_error{"The coroutine did not end"};
            return future.get();
        }

      protected:
        boost::asio::thread_pool pool_{2};
        std::vector<std::shared_ptr<ControlConnection>> connections_{};
        ServerProfile profile_{};
        ServerEndpoint endpoint_{};
    };

    TEST_P(LoginConformance, LogsInAndLearnsTheServer)
    {
        auto connection = makeConnection();
        auto result = login(connection, options());
        if (mode() != TlsMode::None)
        {
            ASSERT_FALSE(result) << "a self-signed certificate was trusted";
            ASSERT_EQ(result.error().kind, RemoteIo::RemoteErrorKind::TrustRequired) << result.error().toString();
            const auto untrusted = connection->untrustedCertificate();
            ASSERT_TRUE(untrusted);
            EXPECT_NE(untrusted->subject.find(fmt::format("CN={}", profile_.image)), std::string::npos)
                << untrusted->subject;
            EXPECT_FALSE(untrusted->verifyFailure.empty());

            connection = makeConnection();
            result = login(connection, options(untrusted->publicKeySha256));
            ASSERT_TRUE(result) << result.error().toString();
            ASSERT_TRUE(result->certificate);
            EXPECT_EQ(result->certificate->source, TrustSource::Pin);
            const auto resumable = onStrand(
                connection,
                [connection]() -> boost::asio::awaitable<bool>
                {
                    co_return connection->sessionForData().has_value();
                }
            );
            EXPECT_TRUE(resumable);
        }
        ASSERT_TRUE(result) << result.error().toString();

        EXPECT_EQ(result->greeting.code, 220);
        EXPECT_TRUE(result->homeDirectory.starts_with('/')) << result->homeDirectory;
        EXPECT_EQ(result->utf8Accepted, profile_.expectations.features.utf8);
        EXPECT_EQ(result->features.has("MLST"), profile_.expectations.features.mlst);
        EXPECT_EQ(result->features.has("MFMT"), profile_.expectations.features.mfmt);
        for (auto const& feature : profile_.expectations.advertised)
        {
            const auto space = feature.find(' ');
            const bool listed = space == std::string::npos
                ? result->features.has(feature)
                : result->features.has(feature.substr(0u, space), feature.substr(space + 1u));
            EXPECT_TRUE(listed) << feature;
        }
    }

    TEST_P(LoginConformance, WrongPasswordFailsTheLogin)
    {
        auto loginOptions = options();
        loginOptions.credentials.password = "not the password";
        if (mode() != TlsMode::None)
        {
            const auto probe = makeConnection();
            const auto refused = login(probe, options());
            ASSERT_FALSE(refused);
            ASSERT_TRUE(probe->untrustedCertificate());
            loginOptions = options(probe->untrustedCertificate()->publicKeySha256);
            loginOptions.credentials.password = "not the password";
        }
        const auto connection = makeConnection();
        const auto result = login(connection, loginOptions);
        ASSERT_FALSE(result) << "logged in with a wrong password";
        EXPECT_EQ(result.error().kind, RemoteIo::RemoteErrorKind::AuthenticationFailed) << result.error().toString();
    }

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        LoginConformance,
        ::testing::Combine(
            ::testing::ValuesIn(selectedServers()),
            ::testing::Values(TlsMode::None, TlsMode::Explicit, TlsMode::Implicit)
        ),
        [](::testing::TestParamInfo<std::tuple<std::string, TlsMode>> const& info)
        {
            const auto mode = std::get<1>(info.param);
            if (mode == TlsMode::Explicit)
                return std::get<0>(info.param) + "Explicit";
            if (mode == TlsMode::Implicit)
                return std::get<0>(info.param) + "Implicit";
            return std::get<0>(info.param) + "Plain";
        }
    );
}

#endif
