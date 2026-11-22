#pragma once

#ifndef _WIN32

#    include "test_pool.hpp"
#    include "test_transfer_conformance.hpp"

#    include <ftp/engine/resolver.hpp>
#    include <ftp/pool/pool.hpp>

#    include <boost/asio/co_spawn.hpp>
#    include <boost/asio/strand.hpp>
#    include <boost/asio/use_future.hpp>
#    include <fmt/format.h>
#    include <gtest/gtest.h>

#    include <chrono>
#    include <future>
#    include <memory>
#    include <optional>
#    include <stdexcept>
#    include <string>
#    include <string_view>
#    include <thread>
#    include <utility>
#    include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief The connection pool against every selected server, plain and over both kinds of TLS. A pool for TLS
     *        starts once to see the certificate and again with it pinned, the way a trust prompt goes.
     */
    class PoolConformance : public LoginConformance
    {
      protected:
        template <typename WorkT>
        static auto run(std::shared_ptr<Pool> const& pool, WorkT work)
        {
            auto future = boost::asio::co_spawn(pool->strand(), std::move(work), boost::asio::use_future);
            if (future.wait_for(60s) != std::future_status::ready)
                throw std::runtime_error{"The coroutine did not end"};
            return future.get();
        }

        void SetUp() override
        {
            LoginConformance::SetUp();
            if (IsSkipped() || HasFatalFailure())
                return;
            auto endpoint = serverEndpoint(server(), variant());
            ASSERT_TRUE(endpoint.has_value()) << endpoint.error();
            endpoint_ = std::move(endpoint).value();
            auto workspace = prepareWorkspace(endpoint_);
            ASSERT_TRUE(workspace.has_value()) << workspace.error();
            workspace_ = std::move(workspace).value();
        }

        void TearDown() override
        {
            for (auto const& pool : pools_)
                run(pool, pool->close());
            LoginConformance::TearDown();
            expectWorkspaceRemoved(endpoint_, workspace_);
        }

        virtual std::string_view variant() const
        {
            return "default";
        }

        virtual PoolOptions poolOptions(std::size_t transferConnections, std::optional<Sha256Digest> pin) const
        {
            return PoolOptions{
                .host = endpoint_.address,
                .port = port(),
                .login = options(pin),
                .control = {.connectTimeout = 10s, .commandTimeout = 10s},
                .transferConnections = transferConnections,
            };
        }

        /**
         * @brief Where the pools and their connections run.
         */
        virtual boost::asio::any_io_executor poolExecutor()
        {
            return pool_.get_executor();
        }

        std::shared_ptr<Pool> makePool(PoolOptions poolOptions)
        {
            auto pool = Pool::create(boost::asio::make_strand(poolExecutor()), resolver_, std::move(poolOptions));
            pools_.push_back(pool);
            return pool;
        }

        /**
         * @brief Starts a pool. A server that refuses it for too many connections still frees the slots of an earlier
         *        case, which it does on its own time, so the start is tried again a few times.
         */
        std::pair<std::shared_ptr<Pool>, std::expected<LoginResult, RemoteIo::RemoteError>>
        tryStart(std::size_t transferConnections, std::optional<Sha256Digest> pin)
        {
            auto pool = makePool(poolOptions(transferConnections, pin));
            auto started = run(pool, pool->start());
            for (
                auto attempt = 1;
                attempt < 20 && !started && started.error().kind == RemoteIo::RemoteErrorKind::ConnectionLimit;
                ++attempt
            )
            {
                std::this_thread::sleep_for(250ms);
                pool = makePool(poolOptions(transferConnections, pin));
                started = run(pool, pool->start());
            }
            return {pool, std::move(started)};
        }

        /**
         * @brief A started pool, for TLS with the certificate its first start refused pinned.
         */
        std::shared_ptr<Pool> startPool(std::size_t transferConnections)
        {
            std::optional<Sha256Digest> pin{};
            if (mode() != TlsMode::None)
            {
                const auto [probe, refused] = tryStart(transferConnections, std::nullopt);
                if (refused || refused.error().kind != RemoteIo::RemoteErrorKind::TrustRequired)
                    throw std::runtime_error{"The start did not ask about the self-signed certificate"};
                const auto untrusted =
                    run(probe,
                        [probe]() -> boost::asio::awaitable<std::optional<CertificateInfo>>
                        {
                            co_return probe->untrustedCertificate();
                        });
                if (!untrusted)
                    throw std::runtime_error{"The refused start kept no certificate"};
                pin = untrusted->publicKeySha256;
            }
            auto [pool, started] = tryStart(transferConnections, pin);
            if (!started)
                throw std::runtime_error{fmt::format("The pool did not start: {}", started.error().toString())};
            return pool;
        }

        static Lease leaseFrom(Pool::LeaseResult result)
        {
            if (!result)
                throw std::runtime_error{fmt::format("No lease: {}", result.error().toString())};
            return std::move(*result);
        }

        std::string pathOf(std::string_view name) const
        {
            return fmt::format("{}/{}", workspace_, name);
        }

        static TransferOptions transferOptions()
        {
            return TransferOptions{
                .data =
                    DataConnectionOptions{.connectTimeout = 10s, .inactivityTimeout = 10s, .closeNotifyTimeout = 5s},
                .finalReplyTimeout = 10s,
            };
        }

        std::future<DownloadResult> downloadOn(Lease const& lease, std::string_view name) const
        {
            return boost::asio::co_spawn(
                lease.control()->strand(),
                downloadAll(lease.transferSession(), DownloadRequest{.path = pathOf(name)}, transferOptions()),
                boost::asio::use_future
            );
        }

        static void expectDownload(std::future<DownloadResult>& future, std::size_t size)
        {
            ASSERT_EQ(future.wait_for(60s), std::future_status::ready) << "the download did not end";
            const auto result = future.get();
            ASSERT_TRUE(result) << result.error().toString();
            EXPECT_EQ(result->content.size(), size);
            EXPECT_TRUE(result->content == fixtureContent(size));
        }

        static std::expected<Reply, RemoteIo::RemoteError> executeOn(Lease const& lease, Command command)
        {
            const auto control = lease.control();
            auto future =
                boost::asio::co_spawn(control->strand(), control->execute(std::move(command)), boost::asio::use_future);
            if (future.wait_for(30s) != std::future_status::ready)
                throw std::runtime_error{"The command did not end"};
            return future.get();
        }

        /**
         * @brief Waits until @p control ended. Only the server's clock or the keepalive decides when, so its state is
         *        asked again until then.
         */
        static bool waitForEnd(std::shared_ptr<ControlConnection> const& control)
        {
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            auto open = true;
            while (open && std::chrono::steady_clock::now() < deadline)
            {
                auto future = boost::asio::co_spawn(
                    control->strand(),
                    [control]() -> boost::asio::awaitable<bool>
                    {
                        co_return control->state() == ControlState::Open;
                    },
                    boost::asio::use_future
                );
                open = future.get();
                if (open)
                    std::this_thread::sleep_for(100ms);
            }
            return !open;
        }

      protected:
        std::shared_ptr<Resolver> resolver_ = std::make_shared<Resolver>();
        std::vector<std::shared_ptr<Pool>> pools_{};
        std::string workspace_{};
    };

    /**
     * @brief The limits variant: three connections from one address, idle sessions end after 5 s. Its cases run one
     *        at a time, they share the count of the address.
     */
    class PoolLimitConformance : public PoolConformance
    {
      protected:
        std::string_view variant() const override
        {
            return "limits";
        }
    };

    TEST_P(PoolConformance, TransfersRunAtOnceOnLoginsOfTheirOwn)
    {
        const auto pool = startPool(2u);
        const auto first = leaseFrom(run(pool, pool->acquireTransfer()));
        const auto second = leaseFrom(run(pool, pool->acquireTransfer()));
        const auto command = leaseFrom(run(pool, pool->acquireCommand()));
        EXPECT_EQ(first.name(), "C1");
        EXPECT_EQ(second.name(), "C2");
        EXPECT_EQ(command.name(), "C0");

        auto one = downloadOn(first, "large.bin");
        auto two = downloadOn(second, "large.bin");
        const auto directory = executeOn(command, Command{.verb = Verb::PrintWorkingDirectory});
        ASSERT_TRUE(directory) << directory.error().toString();
        EXPECT_EQ(directory->code, 257);
        ASSERT_NO_FATAL_FAILURE(expectDownload(one, 10485760u));
        ASSERT_NO_FATAL_FAILURE(expectDownload(two, 10485760u));
    }

    TEST_P(PoolLimitConformance, LearnsTheLimitOfTheServerAndGoesOnWithinIt)
    {
        // Four connections wanted, the server takes three from one address.
        const auto pool = startPool(3u);
        const auto wakes = std::make_shared<WakeCount>();
        std::vector<Lease> leases{};
        std::optional<RemoteIo::RemoteError> refusal{};
        for (auto attempt = 0; attempt < 3 && !refusal; ++attempt)
        {
            auto acquired = run(pool, pool->acquireTransfer(wakes->wake()));
            if (acquired)
                leases.push_back(std::move(*acquired));
            else
                refusal = acquired.error();
        }
        ASSERT_TRUE(refusal) << "the server took four connections";
        EXPECT_EQ(refusal->kind, RemoteIo::RemoteErrorKind::Busy) << refusal->toString();
        ASSERT_FALSE(leases.empty());
        EXPECT_LE(pool->connectionLimit(), 3u);
        EXPECT_EQ(pool->connectionLimit(), leases.size() + 1u);

        auto download = downloadOn(leases.back(), "chunk.bin");
        ASSERT_NO_FATAL_FAILURE(expectDownload(download, 16384u));
        leases.back().release();
        ASSERT_EQ(wakes->woken.wait_for(30s), std::future_status::ready) << "the release woke no transfer";
        const auto again = leaseFrom(run(pool, pool->acquireTransfer()));
        auto next = downloadOn(again, "chunk.bin");
        ASSERT_NO_FATAL_FAILURE(expectDownload(next, 16384u));
    }

    TEST_P(PoolLimitConformance, C0EndedByTheIdleTimeoutLogsInAtItsNextUse)
    {
        const auto pool = startPool(1u);
        auto first = leaseFrom(run(pool, pool->acquireCommand()));
        const auto ended = first.control();
        first.release();
        ASSERT_TRUE(waitForEnd(ended)) << "the server kept the idle session";

        const auto command = leaseFrom(run(pool, pool->acquireCommand()));
        EXPECT_NE(command.control(), ended);
        const auto directory = executeOn(command, Command{.verb = Verb::PrintWorkingDirectory});
        ASSERT_TRUE(directory) << directory.error().toString();
        EXPECT_EQ(directory->code, 257);
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
    }

    namespace
    {
        std::string nameOfPoolCase(::testing::TestParamInfo<std::tuple<std::string, TlsMode>> const& info)
        {
            const auto mode = std::get<1>(info.param);
            if (mode == TlsMode::Explicit)
                return std::get<0>(info.param) + "Explicit";
            if (mode == TlsMode::Implicit)
                return std::get<0>(info.param) + "Implicit";
            return std::get<0>(info.param) + "Plain";
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        PoolConformance,
        ::testing::Combine(
            ::testing::ValuesIn(selectedServers()),
            ::testing::Values(TlsMode::None, TlsMode::Explicit, TlsMode::Implicit)
        ),
        nameOfPoolCase
    );

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        PoolLimitConformance,
        ::testing::Combine(
            ::testing::ValuesIn(selectedServers()),
            ::testing::Values(TlsMode::None, TlsMode::Explicit, TlsMode::Implicit)
        ),
        nameOfPoolCase
    );
}

#endif
