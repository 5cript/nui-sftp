#pragma once

#include <ssh/async/buffer_provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace SecureShell::Test
{
    class BufferProviderTests : public ::testing::Test
    {
      public:
        using Provider = DefaultBufferProvider;
        /** Threshold gating the Max bucket (same as DefaultBufferProvider). */
        static constexpr std::size_t kMaxThreshold = 1ull * 1024ull * 1024ull;
    };

    TEST_F(BufferProviderTests, LeaseHintZeroPicksTiny)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(0);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.category(), BufferCategory::Tiny);
        EXPECT_EQ(lease.size(), bufferCategorySize(BufferCategory::Tiny));
    }

    TEST_F(BufferProviderTests, LeaseHintSmallerThanTinyFitsTiny)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(512);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.category(), BufferCategory::Tiny);
    }

    TEST_F(BufferProviderTests, LeaseHintBetweenTinyAndSmallPicksSmall)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(bufferCategorySize(BufferCategory::Tiny) + 1);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.category(), BufferCategory::Small);
    }

    TEST_F(BufferProviderTests, LeaseHintBetweenSmallAndMediumPicksMedium)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(bufferCategorySize(BufferCategory::Small) + 1);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.category(), BufferCategory::Medium);
    }

    TEST_F(BufferProviderTests, LeaseHintBetweenMediumAndLargePicksLarge)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(bufferCategorySize(BufferCategory::Medium) + 1);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.category(), BufferCategory::Large);
    }

    TEST_F(BufferProviderTests, LeaseBelowThresholdCapsAtLargeEvenWhenHintExceedsLarge)
    {
        auto provider = Provider::create();
        // Hint exceeds Large (64 KiB) but is still well under the 1 MiB threshold gating Max.
        auto lease = provider->lease(bufferCategorySize(BufferCategory::Large) + 1);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.category(), BufferCategory::Large);
    }

    TEST_F(BufferProviderTests, LeaseAtOrAboveThresholdPicksMax)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(kMaxThreshold);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.category(), BufferCategory::Max);
        EXPECT_EQ(lease.size(), bufferCategorySize(BufferCategory::Max));
    }

    TEST_F(BufferProviderTests, LeaseForTransferClampsSizeByServerLimit)
    {
        auto provider = Provider::create();
        const std::size_t serverLimit = 2 * 1024;
        auto lease = provider->leaseForTransfer(bufferCategorySize(BufferCategory::Large), serverLimit);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.size(), serverLimit);
        EXPECT_GE(lease.capacity(), serverLimit);
    }

    TEST_F(BufferProviderTests, LeaseReusesReleasedSlot)
    {
        auto provider = Provider::create();
        char* firstPointer = nullptr;
        {
            auto lease = provider->lease(bufferCategorySize(BufferCategory::Small));
            ASSERT_TRUE(lease);
            firstPointer = lease.data();
        }
        auto second = provider->lease(bufferCategorySize(BufferCategory::Small));
        ASSERT_TRUE(second);
        EXPECT_EQ(second.data(), firstPointer);
    }

    TEST_F(BufferProviderTests, ExhaustingBucketFallsBackOneSmaller)
    {
        auto provider = Provider::create();
        std::vector<BufferLease> medium{};
        for (std::size_t i = 0; i < 8; ++i)
        {
            auto lease = provider->lease(bufferCategorySize(BufferCategory::Medium));
            ASSERT_TRUE(lease) << "Medium slot " << i;
            ASSERT_EQ(lease.category(), BufferCategory::Medium);
            medium.push_back(std::move(lease));
        }

        auto fallback = provider->lease(bufferCategorySize(BufferCategory::Medium));
        ASSERT_TRUE(fallback);
        EXPECT_EQ(fallback.category(), BufferCategory::Small);
        EXPECT_EQ(fallback.size(), bufferCategorySize(BufferCategory::Small));
    }

    TEST_F(BufferProviderTests, FallbackBottomsOutAtEmpty)
    {
        auto provider = Provider::create();
        std::vector<BufferLease> tinies{};
        for (std::size_t i = 0; i < 8; ++i)
        {
            auto lease = provider->lease(0);
            ASSERT_TRUE(lease);
            tinies.push_back(std::move(lease));
        }
        std::vector<BufferLease> smalls{};
        for (std::size_t i = 0; i < 8; ++i)
        {
            auto lease = provider->lease(bufferCategorySize(BufferCategory::Small));
            ASSERT_TRUE(lease);
            ASSERT_EQ(lease.category(), BufferCategory::Small);
            smalls.push_back(std::move(lease));
        }

        auto empty = provider->lease(bufferCategorySize(BufferCategory::Small));
        EXPECT_FALSE(empty);
        EXPECT_TRUE(empty.empty());
        EXPECT_EQ(empty.data(), nullptr);
    }

    TEST_F(BufferProviderTests, RecoveryAfterRelease)
    {
        auto provider = Provider::create();
        std::vector<BufferLease> held{};
        for (std::size_t i = 0; i < 8; ++i)
            held.push_back(provider->lease(bufferCategorySize(BufferCategory::Medium)));

        // Exhausted: next ask falls back to Small.
        auto fallback = provider->lease(bufferCategorySize(BufferCategory::Medium));
        EXPECT_EQ(fallback.category(), BufferCategory::Small);

        // Free one Medium, next same-category ask succeeds in Medium again.
        held.pop_back();
        auto recovered = provider->lease(bufferCategorySize(BufferCategory::Medium));
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.category(), BufferCategory::Medium);
    }

    TEST_F(BufferProviderTests, ProviderOutlivesLeaseSafely)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(bufferCategorySize(BufferCategory::Tiny));
        ASSERT_TRUE(lease);
        provider.reset();
        // Lease destructor now runs with a dead provider — weak_ptr guard must prevent dangling.
        (void)lease;
    }

    TEST_F(BufferProviderTests, MoveConstructorTransfersOwnership)
    {
        auto provider = Provider::create();
        auto original = provider->lease(bufferCategorySize(BufferCategory::Tiny));
        ASSERT_TRUE(original);
        char* ptr = original.data();

        BufferLease moved{std::move(original)};
        EXPECT_TRUE(moved);
        EXPECT_EQ(moved.data(), ptr);
        EXPECT_FALSE(original);
        EXPECT_EQ(original.data(), nullptr);
    }

    TEST_F(BufferProviderTests, MoveAssignmentReturnsPreviousSlot)
    {
        auto provider = Provider::create();
        auto first = provider->lease(bufferCategorySize(BufferCategory::Tiny));
        char* firstPtr = first.data();

        auto second = provider->lease(bufferCategorySize(BufferCategory::Tiny));
        char* secondPtr = second.data();
        ASSERT_NE(firstPtr, secondPtr);

        first = std::move(second);
        EXPECT_EQ(first.data(), secondPtr);

        // The slot formerly held by `first` is now back in the pool. Leasing again must hand it out.
        auto rebind = provider->lease(bufferCategorySize(BufferCategory::Tiny));
        EXPECT_EQ(rebind.data(), firstPtr);
    }

    TEST_F(BufferProviderTests, ResetReleasesEagerly)
    {
        auto provider = Provider::create();
        auto lease = provider->lease(bufferCategorySize(BufferCategory::Tiny));
        ASSERT_TRUE(lease);
        char* ptr = lease.data();

        lease.reset();
        EXPECT_FALSE(lease);

        auto again = provider->lease(bufferCategorySize(BufferCategory::Tiny));
        EXPECT_EQ(again.data(), ptr);
    }

    TEST_F(BufferProviderTests, TinyAndSmallAreInlineInProviderObject)
    {
        // Sanity check that the Tiny and Small buckets occupy real storage inside the provider
        // object (not heap-indirected via std::vector). Total inline bytes must be at least
        // (TinySize * TinyCount) + (SmallSize * SmallCount) = 8 KiB + 32 KiB = 40 KiB.
        constexpr std::size_t expectedInline = (bufferCategorySize(BufferCategory::Tiny) * 8u) +
            (bufferCategorySize(BufferCategory::Small) * 8u);
        EXPECT_GE(sizeof(Provider), expectedInline);
    }

    TEST_F(BufferProviderTests, ConcurrentLeaseReleaseIsSafe)
    {
        auto provider = Provider::create();
        constexpr int threadsCount = 8;
        constexpr int iterationsPerThread = 200;

        std::atomic<int> failures{0};
        std::vector<std::thread> workers{};
        workers.reserve(threadsCount);
        for (int threadIndex = 0; threadIndex < threadsCount; ++threadIndex)
        {
            workers.emplace_back(
                [&provider, &failures]()
                {
                    for (int i = 0; i < iterationsPerThread; ++i)
                    {
                        auto lease = provider->lease(bufferCategorySize(BufferCategory::Small));
                        if (!lease)
                        {
                            ++failures;
                            continue;
                        }
                        // Touch the buffer to catch obvious corruption.
                        lease.data()[0] = static_cast<char>(i & 0x7f);
                    }
                }
            );
        }
        for (auto& w : workers)
            w.join();

        EXPECT_EQ(failures.load(), 0);
    }
}
