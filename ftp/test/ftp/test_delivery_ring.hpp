#pragma once

#include "test_engine.hpp"

#include <ftp/engine/delivery_ring.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        /** @brief Set by a task while it waits in Engine::call(). */
        thread_local bool insideACall{false};

        char patternAt(std::size_t index)
        {
            return static_cast<char>(index % 251u);
        }

        /**
         * @brief A download of size bytes, patternAt() each, read in pieces of at most piece bytes.
         */
        struct FakeDownload
        {
            std::size_t size{0u};
            std::size_t piece{0u};
            std::size_t position{0u};
            std::atomic<std::size_t> reads{0u};
            /** @brief Called with the number of each read before it, on the connection strand. */
            std::function<void(std::size_t)> beforeRead{};
            /** @brief The read with this number fails. */
            std::optional<std::size_t> failingRead{};
        };

        DeliveryRing::Source sourceOf(std::shared_ptr<FakeDownload> download)
        {
            return [download](
                       asio::mutable_buffer buffer
                   ) -> asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
            {
                const auto number = download->reads++;
                if (download->beforeRead)
                    download->beforeRead(number);
                co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
                if (download->failingRead == number)
                {
                    co_return std::unexpected(
                        RemoteIo::RemoteError{
                            .kind = RemoteIo::RemoteErrorKind::ConnectionLost,
                            .message = "The data connection broke",
                        }
                    );
                }
                const auto count = std::min({buffer.size(), download->piece, download->size - download->position});
                auto* const bytes = static_cast<char*>(buffer.data());
                for (std::size_t index = 0u; index < count; ++index)
                    bytes[index] = patternAt(download->position + index);
                download->position += count;
                co_return count;
            };
        }

        /**
         * @brief What the sink of a ring got, on the ProcessingThread.
         */
        struct Received
        {
            std::vector<char> bytes{};
            std::vector<std::size_t> sizes{};
            std::atomic<std::size_t> deliveries{0u};
            bool allOnTheProcessingThread{true};
        };
    }

    /**
     * @brief What the tests of both rings share.
     */
    class RingTests : public EngineTests
    {
      protected:
        /**
         * @brief Waits until every task pushed so far ran, the deliveries or fills of an ended ring among them.
         */
        void flushTasks()
        {
            auto flushed = thread_.pushPromiseTask([]() {});
            valueOf(flushed);
        }

        /**
         * @brief Holds the ProcessingThread, as a long synchronous call does, until @p release is ready.
         */
        void holdTheProcessingThread(std::shared_future<void> release)
        {
            ASSERT_TRUE(thread_.pushTask(
                [release]()
                {
                    release.wait();
                }
            ));
        }

        static void expectPattern(std::vector<char> const& bytes, std::size_t size)
        {
            ASSERT_EQ(bytes.size(), size);
            for (std::size_t index = 0u; index < size; ++index)
                ASSERT_EQ(bytes[index], patternAt(index)) << "at byte " << index;
        }

        const RingOptions options_{.slots = 4u, .chunkSize = 1024u};
        const Engine::Strand connectionStrand_ = asio::make_strand(host_.executor());
    };

    class DeliveryRingTests : public RingTests
    {
      protected:
        DeliveryRing::Sink sinkInto(std::shared_ptr<Received> received)
        {
            return [received, gate = gate_](std::span<char const> bytes)
            {
                ++received->deliveries;
                received->allOnTheProcessingThread = received->allOnTheProcessingThread && gate->onProcessingThread();
                received->bytes.insert(received->bytes.end(), bytes.begin(), bytes.end());
                received->sizes.push_back(bytes.size());
                return true;
            };
        }
    };

    TEST_P(DeliveryRingTests, DeliversEverySlotInOrderOnTheProcessingThread)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = 10u * options_.chunkSize + 123u;
        download->piece = 700u;
        const auto received = std::make_shared<Received>();
        auto ran =
            asio::co_spawn(connectionStrand_, ring->run(sourceOf(download), sinkInto(received)), asio::use_future);
        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        flushTasks();

        ASSERT_NO_FATAL_FAILURE(expectPattern(received->bytes, download->size));
        ASSERT_EQ(received->sizes.size(), 11u);
        for (std::size_t index = 0u; index + 1u < received->sizes.size(); ++index)
            EXPECT_EQ(received->sizes[index], options_.chunkSize) << "delivery " << index << " was not a full slot";
        EXPECT_EQ(received->sizes.back(), 123u);
        EXPECT_TRUE(received->allOnTheProcessingThread);
    }

    TEST_P(DeliveryRingTests, ReadsAtMostItsSlotsAheadOfABlockedProcessingThread)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto received = std::make_shared<Received>();
        const auto download = std::make_shared<FakeDownload>();
        download->size = 20u * options_.chunkSize;
        download->piece = options_.chunkSize;
        std::promise<void> slotsFilling{};
        const auto filling = slotsFilling.get_future().share();
        download->beforeRead = [&slotsFilling, received, slots = options_.slots](std::size_t number)
        {
            // One read fills one slot, the slot of read n is free once delivery n - slots began.
            if (number >= slots)
                EXPECT_GE(received->deliveries.load() + slots, number + 1u) << "read " << number << " had no free slot";
            if (number + 1u == slots)
                slotsFilling.set_value();
        };
        std::promise<void> release{};
        ASSERT_NO_FATAL_FAILURE(holdTheProcessingThread(release.get_future().share()));
        auto ran =
            asio::co_spawn(connectionStrand_, ring->run(sourceOf(download), sinkInto(received)), asio::use_future);
        ASSERT_EQ(filling.wait_for(10s), std::future_status::ready);
        // Only the clock gives a ring without bounds the time to read ahead.
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(download->reads.load(), options_.slots);
        release.set_value();

        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        flushTasks();
        ASSERT_NO_FATAL_FAILURE(expectPattern(received->bytes, download->size));
    }

    TEST_P(DeliveryRingTests, WaitsWhilePausedAndGoesOnAfterAWake)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = 3u * options_.chunkSize;
        download->piece = options_.chunkSize;
        const auto received = std::make_shared<Received>();
        const auto paused = std::make_shared<std::atomic<bool>>(true);
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(download),
                sinkInto(received),
                [paused]()
                {
                    return paused->load();
                }
            ),
            asio::use_future
        );
        // Only the clock tells a read that does not come.
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(download->reads.load(), 0u);
        *paused = false;
        ring->wake();

        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        flushTasks();
        ASSERT_NO_FATAL_FAILURE(expectPattern(received->bytes, download->size));
    }

    TEST_P(DeliveryRingTests, StopEndsAPausedRingAsCancelled)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = options_.chunkSize;
        download->piece = options_.chunkSize;
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(download),
                sinkInto(std::make_shared<Received>()),
                []()
                {
                    return true;
                }
            ),
            asio::use_future
        );
        ring->stop();
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << outcome.error().toString();
        EXPECT_EQ(download->reads.load(), 0u);
    }

    TEST_P(DeliveryRingTests, ACancelOfItsCoroutineStopsTheRing)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = options_.chunkSize;
        download->piece = options_.chunkSize;
        const auto signal = std::make_shared<asio::cancellation_signal>();
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(download),
                sinkInto(std::make_shared<Received>()),
                []()
                {
                    return true;
                }
            ),
            asio::bind_cancellation_slot(signal->slot(), asio::use_future)
        );
        asio::post(
            connectionStrand_,
            [signal]()
            {
                signal->emit(asio::cancellation_type::terminal);
            }
        );
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << outcome.error().toString();
        EXPECT_EQ(download->reads.load(), 0u);
    }

    TEST_P(DeliveryRingTests, CloseInsideADeliveryDropsTheDeliveriesBehindIt)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = std::numeric_limits<std::size_t>::max();
        download->piece = options_.chunkSize;
        std::promise<void> twoPushed{};
        auto pushed = twoPushed.get_future();
        download->beforeRead = [&twoPushed](std::size_t number)
        {
            // One read fills one slot, so before the third read two deliveries were pushed.
            if (number == 2u)
                twoPushed.set_value();
        };
        const auto deliveries = std::make_shared<std::atomic<std::size_t>>(0u);
        std::promise<void> release{};
        ASSERT_NO_FATAL_FAILURE(holdTheProcessingThread(release.get_future().share()));
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(download),
                // Holds the ring as a stream's sink does, the ring lets go of it once it ended.
                [ring, deliveries](std::span<char const>)
                {
                    ++*deliveries;
                    ring->close();
                    return true;
                }
            ),
            asio::use_future
        );
        valueOf(pushed);
        release.set_value();

        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << outcome.error().toString();
        flushTasks();
        EXPECT_EQ(deliveries->load(), 1u);
    }

    TEST_P(DeliveryRingTests, ASinkThatRefusesStopsTheRing)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = std::numeric_limits<std::size_t>::max();
        download->piece = options_.chunkSize;
        std::promise<void> twoPushed{};
        auto pushed = twoPushed.get_future();
        download->beforeRead = [&twoPushed](std::size_t number)
        {
            if (number == 2u)
                twoPushed.set_value();
        };
        const auto deliveries = std::make_shared<std::atomic<std::size_t>>(0u);
        std::promise<void> release{};
        ASSERT_NO_FATAL_FAILURE(holdTheProcessingThread(release.get_future().share()));
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(download),
                [deliveries](std::span<char const>)
                {
                    ++*deliveries;
                    return false;
                }
            ),
            asio::use_future
        );
        valueOf(pushed);
        release.set_value();

        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << outcome.error().toString();
        flushTasks();
        EXPECT_EQ(deliveries->load(), 1u);
    }

    TEST_P(DeliveryRingTests, AFailedReadEndsTheRingWithItsError)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = 10u * options_.chunkSize;
        download->piece = options_.chunkSize;
        download->failingRead = 2u;
        const auto received = std::make_shared<Received>();
        auto ran =
            asio::co_spawn(connectionStrand_, ring->run(sourceOf(download), sinkInto(received)), asio::use_future);
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().message, "The data connection broke");
        flushTasks();
        ASSERT_NO_FATAL_FAILURE(expectPattern(received->bytes, 2u * options_.chunkSize));
    }

    TEST_P(DeliveryRingTests, ARingWhoseDeliveriesAreRefusedEndsAsLost)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = 10u * options_.chunkSize;
        download->piece = options_.chunkSize;
        gate_->close();
        auto ran = asio::co_spawn(
            connectionStrand_, ring->run(sourceOf(download), sinkInto(std::make_shared<Received>())), asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        EXPECT_EQ(download->reads.load(), 1u);
    }

    TEST_P(DeliveryRingTests, NoDeliveryRunsInsideAnotherTasksCall)
    {
        /**
         * @brief Tells the call on the session strand that the ring pushed a delivery behind it.
         */
        struct Handoff
        {
            explicit Handoff(asio::any_io_executor executor)
                : timer{std::move(executor), asio::steady_timer::time_point::max()}
            {}

            asio::steady_timer timer;
            bool pushed{false};
        };

        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        const auto download = std::make_shared<FakeDownload>();
        download->size = 6u * options_.chunkSize;
        download->piece = options_.chunkSize;
        const auto handoff = std::make_shared<Handoff>(engine->strand());
        download->beforeRead = [engine, handoff](std::size_t number)
        {
            // One read fills one slot, so before the second read the first delivery was pushed.
            if (number != 1u)
                return;
            asio::post(
                engine->strand(),
                [handoff]()
                {
                    handoff->pushed = true;
                    handoff->timer.cancel();
                }
            );
        };
        const auto entered = std::make_shared<std::promise<void>>();
        auto inCall = entered->get_future();
        auto task = thread_.pushPromiseTask(
            [engine, handoff, entered]()
            {
                insideACall = true;
                auto result = engine->call<int>(
                    [handoff, entered](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
                    {
                        entered->set_value();
                        while (!handoff->pushed)
                            co_await handoff->timer.async_wait(asio::as_tuple(asio::use_awaitable));
                        co_return 1;
                    },
                    SyncLimits{.backstop = 10s, .drain = 1s}
                );
                insideACall = false;
                return result;
            }
        );
        valueOf(inCall);

        const auto received = std::make_shared<Received>();
        const auto sawACall = std::make_shared<bool>(false);
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(download),
                [sawACall, sink = sinkInto(received)](std::span<char const> bytes)
                {
                    *sawACall = *sawACall || insideACall;
                    return sink(bytes);
                }
            ),
            asio::use_future
        );
        const auto called = valueOf(task);
        ASSERT_TRUE(called) << called.error().toString();
        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        flushTasks();
        EXPECT_FALSE(*sawACall) << "a delivery ran inside the call of another task";
        ASSERT_NO_FATAL_FAILURE(expectPattern(received->bytes, download->size));
    }

    TEST_P(DeliveryRingTests, ACleanupTaskMayCloseTheRingAfterTheSessionClosed)
    {
        const auto engine = makeEngine();
        const auto ring = DeliveryRing::create(engine, connectionStrand_, options_);
        EXPECT_TRUE(engine->close(thread_));
        auto closed = false;
        thread_.pushCleanupTask(
            [&closed, ring]()
            {
                ring->close();
                closed = true;
            }
        );
        EXPECT_TRUE(closed) << "the cleanup task did not run on the closing thread";
    }

    INSTANTIATE_TEST_SUITE_P(
        Executors,
        DeliveryRingTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );
}
