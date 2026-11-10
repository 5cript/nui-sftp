#pragma once

#include <ftp/engine/resolver.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        namespace asio = boost::asio;

        /**
         * @brief A lookup of the host "held" that tells when it started and answers only once the test opens it. Any
         *        other host is an address and answers at once. Its state outlives a resolver thread left behind.
         */
        class HeldLookup
        {
          public:
            HeldLookup() = default;
            /**
             * @brief Opens the lookup, a test that failed early leaves no thread waiting for ever.
             */
            ~HeldLookup()
            {
                open();
            }
            HeldLookup(HeldLookup const&) = delete;
            HeldLookup& operator=(HeldLookup const&) = delete;
            HeldLookup(HeldLookup&&) = delete;
            HeldLookup& operator=(HeldLookup&&) = delete;

            Resolver::Lookup function() const
            {
                return [state = state_](std::string const& host, std::uint16_t port) -> ResolveResult
                {
                    if (host != "held")
                        return Endpoints{{asio::ip::make_address(host), port}};
                    state->started.set_value();
                    state->opened.wait();
                    return Endpoints{{asio::ip::make_address("192.0.2.99"), port}};
                };
            }

            void waitUntilStarted() const
            {
                if (state_->startedFuture.wait_for(std::chrono::seconds{10}) != std::future_status::ready)
                    throw std::runtime_error{"The lookup did not start"};
            }

            void open() const
            {
                std::call_once(
                    state_->opening,
                    [this]()
                    {
                        state_->open.set_value();
                    }
                );
            }

          private:
            struct State
            {
                std::promise<void> started{};
                std::future<void> startedFuture{started.get_future()};
                std::promise<void> open{};
                std::shared_future<void> opened{open.get_future().share()};
                std::once_flag opening{};
            };
            std::shared_ptr<State> state_ = std::make_shared<State>();
        };

        /**
         * @return Nothing when the wait did not end in time.
         */
        template <typename ValueT>
        std::optional<ValueT> waitFor(std::future<ValueT>& future)
        {
            if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready)
                return std::nullopt;
            return future.get();
        }

        std::future<ResolveResult> resolveOn(
            asio::strand<asio::any_io_executor> const& strand,
            Resolver& resolver,
            std::string host,
            asio::cancellation_slot slot = {}
        )
        {
            return asio::co_spawn(
                strand,
                [&resolver, host = std::move(host)]() -> asio::awaitable<ResolveResult>
                {
                    co_return co_await resolver.resolve(host, 21u, asio::use_awaitable);
                },
                asio::bind_cancellation_slot(slot, asio::use_future)
            );
        }
    }

    TEST(ResolverTests, LooksUpOnItsOwnThreadAndAnswersOnTheCallersStrand)
    {
        asio::thread_pool pool{2};
        const auto testThread = std::this_thread::get_id();
        auto lookupThread = testThread;
        Resolver resolver{
            {},
            [&lookupThread](std::string const& host, std::uint16_t port) -> ResolveResult
            {
                lookupThread = std::this_thread::get_id();
                return Endpoints{{asio::ip::make_address(host), port}};
            }
        };
        const auto strand = asio::make_strand(pool.get_executor());
        auto future = asio::co_spawn(
            strand,
            [&resolver, strand]() -> asio::awaitable<std::pair<ResolveResult, bool>>
            {
                auto result = co_await resolver.resolve("192.0.2.1", 2121u, asio::use_awaitable);
                co_return std::pair{std::move(result), strand.running_in_this_thread()};
            },
            asio::use_future
        );
        const auto outcome = waitFor(future);
        ASSERT_TRUE(outcome) << "the wait did not end";
        const auto& [result, onStrand] = *outcome;
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(*result, (Endpoints{{asio::ip::make_address("192.0.2.1"), 2121u}}));
        EXPECT_TRUE(onStrand);
        EXPECT_NE(lookupThread, testThread);
    }

    TEST(ResolverTests, SystemLookupTakesAnAddress)
    {
        const auto result = Resolver::systemLookup("127.0.0.1", 2121u);
        ASSERT_TRUE(result) << result.error().toString();
        EXPECT_EQ(*result, (Endpoints{{asio::ip::make_address("127.0.0.1"), 2121u}}));
    }

    TEST(ResolverTests, SystemLookupOfAnUnknownNameIsNoConnection)
    {
        // RFC 6761 6.4: names in .invalid never resolve.
        const auto result = Resolver::systemLookup("nui-sftp-test.invalid", 21u);
        ASSERT_FALSE(result) << "resolved to " << result->size() << " addresses";
        EXPECT_EQ(result.error().kind, RemoteIo::RemoteErrorKind::NoConnection) << result.error().toString();
    }

    TEST(ResolverTests, FailedLookupIsNoConnection)
    {
        asio::thread_pool pool{2};
        Resolver resolver{
            {},
            [](std::string const&, std::uint16_t) -> ResolveResult
            {
                return std::unexpected(
                    RemoteIo::RemoteError{
                        .kind = RemoteIo::RemoteErrorKind::NoConnection,
                        .message = "Could not look up the server: no such host"
                    }
                );
            }
        };
        auto future = resolveOn(asio::make_strand(pool.get_executor()), resolver, "nowhere");
        const auto result = waitFor(future);
        ASSERT_TRUE(result) << "the wait did not end";
        ASSERT_FALSE(*result);
        EXPECT_EQ(result->error().kind, RemoteIo::RemoteErrorKind::NoConnection) << result->error().toString();
    }

    TEST(ResolverTests, CancelEndsTheWaitWhileTheLookupHangs)
    {
        asio::thread_pool pool{2};
        const HeldLookup held{};
        Resolver resolver{{}, held.function()};
        const auto strand = asio::make_strand(pool.get_executor());
        asio::cancellation_signal signal{};
        auto future = resolveOn(strand, resolver, "held", signal.slot());
        held.waitUntilStarted();
        asio::post(
            strand,
            [&signal]()
            {
                signal.emit(asio::cancellation_type::terminal);
            }
        );
        auto result = waitFor(future);
        // A wait the cancel missed ends with the lookup, before the signal its handler uses goes.
        held.open();
        if (!result)
            result = waitFor(future);
        ASSERT_TRUE(result) << "the wait did not end";
        ASSERT_FALSE(*result) << "the cancel did not end the wait, the lookup did";
        EXPECT_EQ(result->error().kind, RemoteIo::RemoteErrorKind::Cancelled) << result->error().toString();
    }

    TEST(ResolverTests, HungLookupLeavesTheOtherThreadFree)
    {
        asio::thread_pool pool{2};
        const HeldLookup held{};
        Resolver resolver{{.threads = 2u}, held.function()};
        const auto strand = asio::make_strand(pool.get_executor());
        auto hung = resolveOn(strand, resolver, "held");
        held.waitUntilStarted();
        auto other = resolveOn(strand, resolver, "192.0.2.2");
        const auto result = waitFor(other);
        ASSERT_TRUE(result) << "the wait did not end";
        ASSERT_TRUE(*result) << result->error().toString();
        EXPECT_EQ(**result, (Endpoints{{asio::ip::make_address("192.0.2.2"), 21u}}));
        held.open();
        const auto late = waitFor(hung);
        ASSERT_TRUE(late) << "the held wait did not end";
        EXPECT_TRUE(*late) << late->error().toString();
    }

    TEST(ResolverTests, DestructionEndsTheWaitsAndLeavesAHungLookupBehind)
    {
        asio::thread_pool pool{2};
        const HeldLookup held{};
        auto resolver = std::make_unique<Resolver>(ResolverOptions{.threads = 1u}, held.function());
        const auto strand = asio::make_strand(pool.get_executor());
        auto hung = resolveOn(strand, *resolver, "held");
        held.waitUntilStarted();
        auto queued = resolveOn(strand, *resolver, "192.0.2.3");

        resolver.reset();
        for (auto* future : {&hung, &queued})
        {
            const auto result = waitFor(*future);
            ASSERT_TRUE(result) << "a wait did not end";
            ASSERT_FALSE(*result) << "a wait ended with a result";
            EXPECT_EQ(result->error().kind, RemoteIo::RemoteErrorKind::Cancelled) << result->error().toString();
        }
    }
}
