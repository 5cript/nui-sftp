#pragma once

#ifndef _WIN32

#    include "test_pool_conformance.hpp"

#    include <ftp/client/metadata.hpp>
#    include <ftp/support/dropped_connection.hpp>
#    include <ftp/support/executor_probes.hpp>

#    include <boost/asio/buffer.hpp>
#    include <boost/asio/thread_pool.hpp>
#    include <gtest/gtest.h>

#    include <algorithm>
#    include <chrono>
#    include <future>
#    include <memory>
#    include <optional>
#    include <string>
#    include <string_view>
#    include <thread>
#    include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using FaultClock = std::chrono::steady_clock;

        /**
         * @brief Both ends of the control and the data connection of a transfer.
         */
        struct TransferEnds
        {
            boost::asio::ip::tcp::endpoint controlLocal{};
            boost::asio::ip::tcp::endpoint controlRemote{};
            boost::asio::ip::tcp::endpoint dataLocal{};
            boost::asio::ip::tcp::endpoint dataRemote{};
        };

        /**
         * @brief Set once a transfer moved the bytes it was told to, or why it did not.
         */
        using Reached = std::promise<std::expected<TransferEnds, std::string>>;

        struct WatchedTransfer
        {
            /** @brief What a download read. */
            std::string content{};
            std::uint64_t bytes{0u};
            /** @brief Why the transfer failed, nothing once finish() succeeded. */
            std::optional<RemoteIo::RemoteError> failure{};
            /** @brief finish() had the server's final reply. */
            bool finalReply{false};
            /** @brief When the data ended or failed. */
            FaultClock::time_point dataEnded{};
            FaultClock::time_point ended{};
        };

        TransferEnds endsOf(ControlConnection const& control, DataConnection const& data)
        {
            return TransferEnds{
                .controlLocal = control.localEndpoint(),
                .controlRemote = control.remoteEndpoint(),
                .dataLocal = data.localEndpoint(),
                .dataRemote = data.remoteEndpoint(),
            };
        }

        boost::asio::awaitable<WatchedTransfer> watchDownload(
            TransferSession session,
            DownloadRequest request,
            TransferOptions options,
            std::uint64_t reachAfter,
            std::shared_ptr<Reached> reached
        )
        {
            WatchedTransfer watched{};
            auto started = co_await Download::start(session, std::move(request), options);
            if (!started)
            {
                watched.failure = started.error();
                reached->set_value(std::unexpected(started.error().toString()));
                watched.dataEnded = watched.ended = FaultClock::now();
                co_return watched;
            }
            auto const download = std::move(started).value();
            std::vector<char> buffer(64u * 1024u);
            auto ended = false;
            auto signalled = false;
            while (!ended)
            {
                const auto read = co_await download->readSome(boost::asio::buffer(buffer));
                if (!read)
                    watched.failure = read.error();
                else
                    watched.content.append(buffer.data(), *read);
                ended = !read || *read == 0u;
                if (!signalled && watched.content.size() >= reachAfter)
                {
                    signalled = true;
                    reached->set_value(endsOf(*session.control, *download->dataConnection()));
                }
            }
            watched.bytes = watched.content.size();
            watched.dataEnded = FaultClock::now();
            if (!signalled)
                reached->set_value(std::unexpected("The download ended before it got that far"));
            if (!watched.failure)
            {
                const auto finished = co_await download->finish();
                if (finished)
                    watched.finalReply = finished->finalReply.has_value();
                else
                    watched.failure = finished.error();
            }
            watched.ended = FaultClock::now();
            co_return watched;
        }

        boost::asio::awaitable<WatchedTransfer> watchUpload(
            TransferSession session,
            UploadRequest request,
            std::string content,
            TransferOptions options,
            std::uint64_t reachAfter,
            std::shared_ptr<Reached> reached
        )
        {
            WatchedTransfer watched{};
            auto started = co_await Upload::start(session, std::move(request), options);
            if (!started)
            {
                watched.failure = started.error();
                reached->set_value(std::unexpected(started.error().toString()));
                watched.dataEnded = watched.ended = FaultClock::now();
                co_return watched;
            }
            auto const upload = std::move(started).value();
            constexpr std::size_t chunk = 64u * 1024u;
            auto signalled = false;
            while (!watched.failure && watched.bytes < content.size())
            {
                const auto size = std::min<std::size_t>(chunk, content.size() - watched.bytes);
                const auto written = co_await upload->write(boost::asio::buffer(content.data() + watched.bytes, size));
                if (written)
                    watched.bytes += size;
                else
                    watched.failure = written.error();
                if (!signalled && watched.bytes >= reachAfter)
                {
                    signalled = true;
                    reached->set_value(endsOf(*session.control, *upload->dataConnection()));
                }
            }
            if (!signalled)
                reached->set_value(std::unexpected("The upload ended before it got that far"));
            if (!watched.failure)
            {
                const auto finished = co_await upload->finish();
                if (finished)
                    watched.finalReply = true;
                else
                    watched.failure = finished.error();
            }
            watched.dataEnded = watched.ended = FaultClock::now();
            co_return watched;
        }
    }

    /**
     * @brief Connections dropped without a FIN or a reset, in the throttled variant: transfers run at 2 MiB/s, the
     *        server's send buffer holds 64 KiB, so a transfer stalls as soon as nobody reads, and the server ends a
     *        data connection stalled for 3 s. The pools run on one thread, which probes watch, so a connection that
     *        stalls for seconds must never hold it.
     */
    class FaultConformance : public PoolConformance
    {
      protected:
        static constexpr std::size_t largeSize = 10485760u;
        static constexpr std::uint64_t dropAfter = 1024u * 1024u;
        /** @brief Far above what a fault stalls the thread for, its shortest stall takes seconds. */
        static constexpr std::chrono::milliseconds threadHeldLimit{50};
        /** @brief Far above the keepalive's 3 s, far below the final reply timeout. */
        static constexpr std::chrono::seconds keepaliveEndLimit{20};

        std::string_view variant() const override
        {
            return "throttled";
        }

        boost::asio::any_io_executor poolExecutor() override
        {
            return thread_.get_executor();
        }

        /**
         * @brief A keepalive that notices a dropped control connection 3 s after its last packet.
         */
        PoolOptions poolOptions(std::size_t transferConnections, std::optional<Sha256Digest> pin) const override
        {
            auto options = PoolConformance::poolOptions(transferConnections, pin);
            options.control.socket.keepalive = KeepaliveOptions{.idle = 1s, .interval = 1s, .count = 2};
            return options;
        }

        /**
         * @brief The final reply waits longer than any case runs, only the keepalive may end a dropped control
         *        connection.
         */
        static TransferOptions faultTransferOptions()
        {
            return TransferOptions{
                .data = DataConnectionOptions{.connectTimeout = 10s, .inactivityTimeout = 2s, .closeNotifyTimeout = 5s},
                .finalReplyTimeout = 60s,
            };
        }

        void SetUp() override
        {
            PoolConformance::SetUp();
            if (IsSkipped() || HasFatalFailure())
                return;
            probes_.emplace(thread_.get_executor(), 10ms);
        }

        void TearDown() override
        {
            probes_.reset();
            PoolConformance::TearDown();
            // The pools hold sockets of the thread, which goes before the members of the base.
            pools_.clear();
        }

        std::future<WatchedTransfer>
        watchDownloadOn(Lease const& lease, std::string_view name, std::shared_ptr<Reached> reached) const
        {
            return boost::asio::co_spawn(
                lease.control()->strand(),
                watchDownload(
                    lease.transferSession(),
                    DownloadRequest{.path = pathOf(name), .expectedSize = largeSize},
                    faultTransferOptions(),
                    dropAfter,
                    std::move(reached)
                ),
                boost::asio::use_future
            );
        }

        std::future<WatchedTransfer>
        watchUploadOn(Lease const& lease, std::string_view name, std::shared_ptr<Reached> reached) const
        {
            return boost::asio::co_spawn(
                lease.control()->strand(),
                watchUpload(
                    lease.transferSession(),
                    UploadRequest{.path = pathOf(name)},
                    uploadContent(largeSize),
                    faultTransferOptions(),
                    dropAfter,
                    std::move(reached)
                ),
                boost::asio::use_future
            );
        }

        /**
         * @brief A download beside the victim, nobody waits for it to reach anything.
         */
        std::future<WatchedTransfer> bystanderOn(Lease const& lease) const
        {
            return watchDownloadOn(lease, "large.bin", std::make_shared<Reached>());
        }

        static std::expected<TransferEnds, std::string> awaitReached(std::shared_ptr<Reached> const& reached)
        {
            auto future = reached->get_future();
            if (future.wait_for(60s) != std::future_status::ready)
                return std::unexpected("The transfer did not get that far");
            return future.get();
        }

        static WatchedTransfer awaitTransfer(std::future<WatchedTransfer>& future)
        {
            if (future.wait_for(120s) != std::future_status::ready)
                throw std::runtime_error{"The transfer did not end"};
            return future.get();
        }

        DroppedConnection drop(std::uint16_t serverPort, std::uint16_t clientPort) const
        {
            auto dropped = DroppedConnection::drop(endpoint_.container, serverPort, clientPort);
            if (!dropped)
                throw std::runtime_error{dropped.error()};
            return std::move(dropped).value();
        }

        /**
         * @brief Lists the workspace on @p lease, when it answered.
         */
        std::pair<std::expected<Listing, RemoteIo::RemoteError>, FaultClock::time_point>
        browse(Lease const& lease) const
        {
            auto future = boost::asio::co_spawn(
                lease.control()->strand(),
                listDirectory(
                    MetadataSession{.transfer = lease.transferSession(), .options = faultTransferOptions()}, workspace_
                ),
                boost::asio::use_future
            );
            if (future.wait_for(60s) != std::future_status::ready)
                throw std::runtime_error{"The listing did not end"};
            auto listing = future.get();
            return {std::move(listing), FaultClock::now()};
        }

        static void expectComplete(WatchedTransfer const& transfer)
        {
            ASSERT_FALSE(transfer.failure) << transfer.failure->toString();
            EXPECT_EQ(transfer.content.size(), largeSize);
            EXPECT_TRUE(transfer.content == fixtureContent(largeSize));
            EXPECT_TRUE(transfer.finalReply);
        }

        /**
         * @brief The session goes on: a transfer gets a connection, a new one in place of a dropped one.
         */
        void expectSessionGoesOn(std::shared_ptr<Pool> const& pool) const
        {
            const auto wakes = std::make_shared<WakeCount>();
            auto acquired = run(pool, pool->acquireTransfer(wakes->wake()));
            if (!acquired && acquired.error().kind == RemoteIo::RemoteErrorKind::Busy)
            {
                // The leases given back before reach the pool on its strand.
                ASSERT_EQ(wakes->woken.wait_for(30s), std::future_status::ready) << "no release woke the transfer";
                acquired = run(pool, pool->acquireTransfer());
            }
            const auto next = leaseFrom(std::move(acquired));
            auto download = downloadOn(next, "chunk.bin");
            ASSERT_NO_FATAL_FAILURE(expectDownload(download, 16384u));
            EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        }

        void expectThreadNeverHeld()
        {
            probes_->stop();
            EXPECT_GT(probes_->timerTicks(), 0u);
            EXPECT_LT(probes_->worstTimerLateness(), threadHeldLimit);
            EXPECT_LT(probes_->worstWakeLatency(), threadHeldLimit);
        }

      private:
        boost::asio::thread_pool thread_{1u};
        std::optional<ExecutorProbes> probes_{};
    };

    TEST_P(FaultConformance, ControlDroppedMidDownloadEndsByTheKeepalive)
    {
        const auto pool = startPool(2u);
        auto victimLease = leaseFrom(run(pool, pool->acquireTransfer()));
        auto bystanderLease = leaseFrom(run(pool, pool->acquireTransfer()));
        const auto command = leaseFrom(run(pool, pool->acquireCommand()));
        const auto reached = std::make_shared<Reached>();
        auto victim = watchDownloadOn(victimLease, "large.bin", reached);
        auto bystander = bystanderOn(bystanderLease);
        const auto ends = awaitReached(reached);
        ASSERT_TRUE(ends) << ends.error();

        const auto dropped = drop(ends->controlRemote.port(), ends->controlLocal.port());
        const auto droppedAt = FaultClock::now();
        const auto [listing, listedAt] = browse(command);
        ASSERT_TRUE(listing) << listing.error().toString();

        const auto victimEnd = awaitTransfer(victim);
        // The data was complete, only the final reply is missing: its size proves the download.
        ASSERT_FALSE(victimEnd.failure) << victimEnd.failure->toString();
        EXPECT_FALSE(victimEnd.finalReply);
        EXPECT_TRUE(victimEnd.content == fixtureContent(largeSize));
        EXPECT_LT(victimEnd.ended - std::max(droppedAt, victimEnd.dataEnded), keepaliveEndLimit);
        EXPECT_LT(listedAt, victimEnd.ended) << "the listing waited for the dropped connection";
        ASSERT_NO_FATAL_FAILURE(expectComplete(awaitTransfer(bystander)));

        victimLease.release();
        bystanderLease.release();
        ASSERT_NO_FATAL_FAILURE(expectSessionGoesOn(pool));
        expectThreadNeverHeld();
    }

    TEST_P(FaultConformance, ControlDroppedMidUploadEndsByTheKeepalive)
    {
        const auto pool = startPool(2u);
        auto victimLease = leaseFrom(run(pool, pool->acquireTransfer()));
        auto bystanderLease = leaseFrom(run(pool, pool->acquireTransfer()));
        const auto command = leaseFrom(run(pool, pool->acquireCommand()));
        const auto reached = std::make_shared<Reached>();
        auto victim = watchUploadOn(victimLease, "dropped-control.bin", reached);
        auto bystander = bystanderOn(bystanderLease);
        const auto ends = awaitReached(reached);
        ASSERT_TRUE(ends) << ends.error();

        const auto dropped = drop(ends->controlRemote.port(), ends->controlLocal.port());
        const auto droppedAt = FaultClock::now();
        const auto [listing, listedAt] = browse(command);
        ASSERT_TRUE(listing) << listing.error().toString();

        const auto victimEnd = awaitTransfer(victim);
        // Without the final reply nothing confirms the upload.
        ASSERT_TRUE(victimEnd.failure);
        EXPECT_EQ(victimEnd.failure->kind, RemoteIo::RemoteErrorKind::TimedOut) << victimEnd.failure->toString();
        EXPECT_LT(victimEnd.ended - droppedAt, keepaliveEndLimit);
        EXPECT_LT(listedAt, victimEnd.ended) << "the listing waited for the dropped connection";
        ASSERT_NO_FATAL_FAILURE(expectComplete(awaitTransfer(bystander)));

        victimLease.release();
        bystanderLease.release();
        ASSERT_NO_FATAL_FAILURE(expectSessionGoesOn(pool));
        expectThreadNeverHeld();
    }

    TEST_P(FaultConformance, DataDroppedMidDownloadEndsByTheInactivityDeadline)
    {
        const auto pool = startPool(2u);
        auto victimLease = leaseFrom(run(pool, pool->acquireTransfer()));
        auto bystanderLease = leaseFrom(run(pool, pool->acquireTransfer()));
        const auto command = leaseFrom(run(pool, pool->acquireCommand()));
        const auto reached = std::make_shared<Reached>();
        auto victim = watchDownloadOn(victimLease, "large.bin", reached);
        auto bystander = bystanderOn(bystanderLease);
        const auto ends = awaitReached(reached);
        ASSERT_TRUE(ends) << ends.error();

        const auto dropped = drop(ends->dataRemote.port(), ends->dataLocal.port());
        const auto [listing, listedAt] = browse(command);
        ASSERT_TRUE(listing) << listing.error().toString();

        const auto victimEnd = awaitTransfer(victim);
        ASSERT_TRUE(victimEnd.failure);
        EXPECT_EQ(victimEnd.failure->kind, RemoteIo::RemoteErrorKind::TimedOut) << victimEnd.failure->toString();
        EXPECT_LT(victimEnd.content.size(), largeSize);
        EXPECT_LT(listedAt, victimEnd.ended) << "the listing waited for the dropped connection";
        ASSERT_NO_FATAL_FAILURE(expectComplete(awaitTransfer(bystander)));

        // The abort behind the failure leaves the control connection usable or ends it, the pool takes either.
        victimLease.release();
        bystanderLease.release();
        ASSERT_NO_FATAL_FAILURE(expectSessionGoesOn(pool));
        expectThreadNeverHeld();
    }

    TEST_P(FaultConformance, IdleCommandConnectionDroppedLogsInAtItsNextUse)
    {
        const auto pool = startPool(1u);
        auto first = leaseFrom(run(pool, pool->acquireCommand()));
        const auto dropped = first.control();
        const auto droppedConnection = drop(dropped->remoteEndpoint().port(), dropped->localEndpoint().port());
        first.release();
        ASSERT_TRUE(waitForEnd(dropped)) << "the keepalive did not notice the dropped connection";

        const auto command = leaseFrom(run(pool, pool->acquireCommand()));
        EXPECT_NE(command.control(), dropped);
        const auto [listing, listedAt] = browse(command);
        ASSERT_TRUE(listing) << listing.error().toString();
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        expectThreadNeverHeld();
    }

    TEST_P(FaultConformance, PauseBeyondTheServersDataTimeoutContinuesWithRest)
    {
        const auto pool = startPool(1u);
        const auto lease = leaseFrom(run(pool, pool->acquireTransfer()));
        ASSERT_TRUE(lease.transferSession().restartStream) << "the server does not list REST STREAM";
        auto options = faultTransferOptions();
        options.pauseDropAfter = 1s;
        const auto control = lease.control();
        const auto onControl = [&control](auto work)
        {
            auto future = boost::asio::co_spawn(control->strand(), std::move(work), boost::asio::use_future);
            if (future.wait_for(60s) != std::future_status::ready)
                throw std::runtime_error{"The coroutine did not end"};
            return future.get();
        };
        auto started = onControl(
            Download::start(
                lease.transferSession(),
                DownloadRequest{.path = pathOf("large.bin"), .expectedSize = largeSize},
                options
            )
        );
        ASSERT_TRUE(started) << started.error().toString();
        auto const download = std::move(started).value();
        const auto first = onControl(readPart(download, 64u * 1024u));
        ASSERT_TRUE(first) << first.error().toString();

        // Only the clock tells this case from a short pause: the servers end a data connection stalled for 3 s, vsftpd
        // within 6 s and the whole session with it.
        std::this_thread::sleep_for(8s);

        const auto rest = onControl(readPart(download, largeSize));
        ASSERT_TRUE(rest) << rest.error().toString();
        EXPECT_TRUE(first->content + rest->content == fixtureContent(largeSize));
        const auto finished = onControl(download->finish());
        ASSERT_TRUE(finished) << finished.error().toString();
        ASSERT_TRUE(finished->finalReply);
        EXPECT_EQ(finished->finalReply->code / 100, 2);
        const auto directory = executeOn(lease, Command{.verb = Verb::PrintWorkingDirectory});
        ASSERT_TRUE(directory) << directory.error().toString();
        EXPECT_EQ(directory->code, 257);
        expectThreadNeverHeld();
    }

    INSTANTIATE_TEST_SUITE_P(
        Conformance,
        FaultConformance,
        ::testing::Combine(
            ::testing::ValuesIn(selectedServers()),
            ::testing::Values(TlsMode::None, TlsMode::Explicit, TlsMode::Implicit)
        ),
        nameOfPoolCase
    );
}

#endif
