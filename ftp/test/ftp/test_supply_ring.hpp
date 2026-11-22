#pragma once

#include "test_delivery_ring.hpp"

#include <ftp/engine/supply_ring.hpp>
#include <ftp/engine/task_gate.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        /**
         * @brief The local file of an upload: size bytes, patternAt() each, given in pieces of at most piece bytes.
         */
        struct FakeFile
        {
            std::size_t size{0u};
            std::size_t piece{0u};
            std::size_t position{0u};
            std::atomic<std::size_t> reads{0u};
            bool allOnTheProcessingThread{true};
            /** @brief Called with the number of each read before it, on the ProcessingThread. */
            std::function<void(std::size_t)> beforeRead{};
            /** @brief The read with this number fails. */
            std::optional<std::size_t> failingRead{};
        };

        SupplyRing::Source sourceOf(std::shared_ptr<FakeFile> file, std::shared_ptr<TaskGate> gate)
        {
            return [file, gate](std::span<char> bytes) noexcept -> std::expected<std::size_t, RemoteIo::RemoteError>
            {
                const auto number = file->reads++;
                file->allOnTheProcessingThread = file->allOnTheProcessingThread && gate->onProcessingThread();
                if (file->beforeRead)
                    file->beforeRead(number);
                if (file->failingRead == number)
                {
                    return std::unexpected(
                        RemoteIo::RemoteError{
                            .kind = RemoteIo::RemoteErrorKind::Failure,
                            .message = "The local file could not be read",
                        }
                    );
                }
                const auto count = std::min({bytes.size(), file->piece, file->size - file->position});
                for (std::size_t index = 0u; index < count; ++index)
                    bytes[index] = patternAt(file->position + index);
                file->position += count;
                return count;
            };
        }

        /**
         * @brief What the sink of a ring wrote, on the connection strand.
         */
        struct Written
        {
            std::vector<char> bytes{};
            std::vector<std::size_t> sizes{};
            std::atomic<std::size_t> writes{0u};
            bool allOnTheStrand{true};
            /** @brief Holds the first write until it is cancelled on the connection strand. */
            std::shared_ptr<asio::steady_timer> firstWriteHeld{};
            /** @brief The write with this number fails. */
            std::optional<std::size_t> failingWrite{};
        };

        SupplyRing::Sink sinkInto(std::shared_ptr<Written> written, Engine::Strand strand)
        {
            return [written,
                       strand](asio::const_buffer buffer) -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
            {
                const auto number = written->writes.load();
                written->allOnTheStrand = written->allOnTheStrand && strand.running_in_this_thread();
                if (number == 0u && written->firstWriteHeld)
                    co_await written->firstWriteHeld->async_wait(asio::as_tuple(asio::use_awaitable));
                co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
                if (written->failingWrite == number)
                {
                    co_return std::unexpected(
                        RemoteIo::RemoteError{
                            .kind = RemoteIo::RemoteErrorKind::ConnectionLost,
                            .message = "The data connection broke",
                        }
                    );
                }
                auto const* const bytes = static_cast<char const*>(buffer.data());
                written->bytes.insert(written->bytes.end(), bytes, bytes + buffer.size());
                written->sizes.push_back(buffer.size());
                ++written->writes;
                co_return std::expected<void, RemoteIo::RemoteError>{};
            };
        }
    }

    class SupplyRingTests : public RingTests
    {};

    TEST_P(SupplyRingTests, WritesEverySlotInOrderOnTheConnectionStrand)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 10u * options_.chunkSize + 123u;
        file->piece = 700u;
        const auto written = std::make_shared<Written>();
        auto ran = asio::co_spawn(
            connectionStrand_, ring->run(sourceOf(file, gate_), sinkInto(written, connectionStrand_)), asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        flushTasks();

        ASSERT_NO_FATAL_FAILURE(expectPattern(written->bytes, file->size));
        ASSERT_EQ(written->sizes.size(), 11u);
        for (std::size_t index = 0u; index + 1u < written->sizes.size(); ++index)
            EXPECT_EQ(written->sizes[index], options_.chunkSize) << "write " << index << " was not a full slot";
        EXPECT_EQ(written->sizes.back(), 123u);
        EXPECT_TRUE(written->allOnTheStrand);
        EXPECT_TRUE(file->allOnTheProcessingThread);
        // Two reads fill a slot, the last fill reads the 123 bytes and the end; none reads after the end.
        EXPECT_EQ(file->reads.load(), 10u * 2u + 2u);
    }

    TEST_P(SupplyRingTests, FillsAtMostItsSlotsAheadOfAStalledWrite)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto written = std::make_shared<Written>();
        written->firstWriteHeld =
            std::make_shared<asio::steady_timer>(connectionStrand_, asio::steady_timer::time_point::max());
        const auto file = std::make_shared<FakeFile>();
        file->size = 20u * options_.chunkSize;
        file->piece = options_.chunkSize;
        std::promise<void> slotsFilling{};
        const auto filling = slotsFilling.get_future().share();
        file->beforeRead = [&slotsFilling, written, slots = options_.slots](std::size_t number)
        {
            // One read fills one slot, the slot of read n is free once write n - slots ended.
            if (number >= slots)
                EXPECT_GE(written->writes.load() + slots, number + 1u) << "read " << number << " had no free slot";
            if (number + 1u == slots)
                slotsFilling.set_value();
        };
        auto ran = asio::co_spawn(
            connectionStrand_, ring->run(sourceOf(file, gate_), sinkInto(written, connectionStrand_)), asio::use_future
        );
        ASSERT_EQ(filling.wait_for(10s), std::future_status::ready);
        // Only the clock gives a ring without bounds the time to fill ahead.
        std::this_thread::sleep_for(50ms);
        flushTasks();
        EXPECT_EQ(file->reads.load(), options_.slots);
        asio::post(
            connectionStrand_,
            [held = written->firstWriteHeld]()
            {
                held->cancel();
            }
        );

        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectPattern(written->bytes, file->size));
    }

    TEST_P(SupplyRingTests, WaitsWhilePausedAndGoesOnAfterAWake)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 3u * options_.chunkSize;
        file->piece = options_.chunkSize;
        const auto written = std::make_shared<Written>();
        const auto paused = std::make_shared<std::atomic<bool>>(true);
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(file, gate_),
                sinkInto(written, connectionStrand_),
                [paused]()
                {
                    return paused->load();
                }
            ),
            asio::use_future
        );
        // The ring looks at the pause in its first run on the strand, where it would push its fills.
        flushStrand();
        flushTasks();
        EXPECT_EQ(file->reads.load(), 0u);
        *paused = false;
        ring->wake();

        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectPattern(written->bytes, file->size));
    }

    TEST_P(SupplyRingTests, StopEndsAPausedRingAsCancelled)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = options_.chunkSize;
        file->piece = options_.chunkSize;
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(file, gate_),
                sinkInto(std::make_shared<Written>(), connectionStrand_),
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
        flushTasks();
        EXPECT_EQ(file->reads.load(), 0u);
    }

    TEST_P(SupplyRingTests, ACancelOfItsCoroutineStopsTheRing)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = options_.chunkSize;
        file->piece = options_.chunkSize;
        const auto signal = std::make_shared<asio::cancellation_signal>();
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                sourceOf(file, gate_),
                sinkInto(std::make_shared<Written>(), connectionStrand_),
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
        flushTasks();
        EXPECT_EQ(file->reads.load(), 0u);
    }

    TEST_P(SupplyRingTests, CloseInsideTheSourceEndsTheFills)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 20u * options_.chunkSize;
        file->piece = options_.chunkSize / 2u;
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                // Holds the ring as a stream's source does, the ring lets go of it once it ended.
                [ring, read = sourceOf(file, gate_)](std::span<char> bytes) noexcept
                {
                    auto got = read(bytes);
                    ring->close();
                    return got;
                },
                sinkInto(std::make_shared<Written>(), connectionStrand_)
            ),
            asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::Cancelled) << outcome.error().toString();
        flushTasks();
        EXPECT_EQ(file->reads.load(), 1u);
    }

    TEST_P(SupplyRingTests, AFailedFillEndsTheRingWithItsError)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 10u * options_.chunkSize;
        file->piece = options_.chunkSize;
        file->failingRead = 2u;
        const auto written = std::make_shared<Written>();
        auto ran = asio::co_spawn(
            connectionStrand_, ring->run(sourceOf(file, gate_), sinkInto(written, connectionStrand_)), asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().message, "The local file could not be read");
        EXPECT_LE(written->bytes.size(), 2u * options_.chunkSize);
        ASSERT_NO_FATAL_FAILURE(expectPattern(written->bytes, written->bytes.size()));
        flushTasks();
        // One read fills one slot: the two before the failed one, then none.
        EXPECT_EQ(file->reads.load(), 3u) << "a fill read from the source after it failed";
    }

    TEST_P(SupplyRingTests, AFailedWriteEndsTheRingWithItsError)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 10u * options_.chunkSize;
        file->piece = options_.chunkSize;
        const auto written = std::make_shared<Written>();
        written->failingWrite = 1u;
        auto ran = asio::co_spawn(
            connectionStrand_, ring->run(sourceOf(file, gate_), sinkInto(written, connectionStrand_)), asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().message, "The data connection broke");
        ASSERT_NO_FATAL_FAILURE(expectPattern(written->bytes, options_.chunkSize));
    }

    TEST_P(SupplyRingTests, ARingWhoseFillsAreRefusedEndsAsLost)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 10u * options_.chunkSize;
        file->piece = options_.chunkSize;
        gate_->close();
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(sourceOf(file, gate_), sinkInto(std::make_shared<Written>(), connectionStrand_)),
            asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        EXPECT_EQ(file->reads.load(), 0u);
    }

    TEST_P(SupplyRingTests, AnEmptyUploadEndsWithoutAWrite)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->piece = options_.chunkSize;
        const auto written = std::make_shared<Written>();
        auto ran = asio::co_spawn(
            connectionStrand_, ring->run(sourceOf(file, gate_), sinkInto(written, connectionStrand_)), asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        EXPECT_EQ(written->writes.load(), 0u);
    }

    TEST_P(SupplyRingTests, ASinkThatThrowsStillLetsGoOfTheSource)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 10u * options_.chunkSize;
        file->piece = options_.chunkSize;
        auto held = std::make_shared<int>(0);
        const std::weak_ptr<int> sourceState = held;
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                [held = std::move(held), read = sourceOf(file, gate_)](std::span<char> bytes) noexcept
                {
                    return read(bytes);
                },
                [](asio::const_buffer) -> asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
                {
                    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
                    throw std::runtime_error{"The sink broke"};
                }
            ),
            asio::use_future
        );
        EXPECT_THROW(std::ignore = valueOf(ran), std::runtime_error);
        flushTasks();
        auto takenBack = asio::post(connectionStrand_, asio::use_future);
        valueOf(takenBack);
        EXPECT_TRUE(sourceState.expired()) << "the ring kept the source after its sink threw";
    }

    TEST_P(SupplyRingTests, FillsTheProcessingThreadDropsEndTheRingAsLost)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        const auto file = std::make_shared<FakeFile>();
        file->size = 10u * options_.chunkSize;
        file->piece = options_.chunkSize;
        auto held = std::make_shared<int>(0);
        const std::weak_ptr<int> sourceState = held;
        std::promise<void> release{};
        ASSERT_NO_FATAL_FAILURE(breakTheProcessingThreadAfter(release.get_future().share()));
        auto ran = asio::co_spawn(
            connectionStrand_,
            ring->run(
                [held = std::move(held), read = sourceOf(file, gate_)](std::span<char> bytes) noexcept
                {
                    return read(bytes);
                },
                sinkInto(std::make_shared<Written>(), connectionStrand_)
            ),
            asio::use_future
        );
        // The ring hands out its slots in its first run on the strand.
        flushStrand();
        release.set_value();

        const auto outcome = valueOf(ran);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        settleTasks();
        EXPECT_EQ(file->reads.load(), 0u);
        EXPECT_TRUE(sourceState.expired()) << "the ring kept the source after its fills were dropped";
    }

    TEST(SupplyRingSourceTests, OnlyASourceDeclaredNoexceptIsTaken)
    {
        const auto mayThrow = [](std::span<char>) -> std::expected<std::size_t, RemoteIo::RemoteError>
        {
            return 0u;
        };
        const auto declared = [](std::span<char>) noexcept -> std::expected<std::size_t, RemoteIo::RemoteError>
        {
            return 0u;
        };
        EXPECT_FALSE((std::is_constructible_v<SupplyRing::Source, decltype(mayThrow)>));
        EXPECT_TRUE((std::is_constructible_v<SupplyRing::Source, decltype(declared)>));
    }

    TEST_P(SupplyRingTests, RunsWhenOnlyItsCoroutineHoldsTheRing)
    {
        const auto engine = makeEngine();
        const auto file = std::make_shared<FakeFile>();
        file->size = 3u * options_.chunkSize + 5u;
        file->piece = options_.chunkSize;
        const auto written = std::make_shared<Written>();
        // The coroutine starts on the strand after the ring made here was let go of.
        auto ran = asio::co_spawn(
            connectionStrand_,
            SupplyRing::create(engine, connectionStrand_, options_)
                ->run(sourceOf(file, gate_), sinkInto(written, connectionStrand_)),
            asio::use_future
        );
        const auto outcome = valueOf(ran);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        flushTasks();
        ASSERT_NO_FATAL_FAILURE(expectPattern(written->bytes, file->size));
    }

    TEST_P(SupplyRingTests, ACleanupTaskMayCloseTheRingAfterTheSessionClosed)
    {
        const auto engine = makeEngine();
        const auto ring = SupplyRing::create(engine, connectionStrand_, options_);
        EXPECT_TRUE(engine->close());
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
        SupplyRingTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );
}
