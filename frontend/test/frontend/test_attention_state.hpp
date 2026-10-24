#pragma once

#include <frontend/session_components/file_tracking/attention_state.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>

namespace FileTracking
{
    inline void PrintTo(AttentionCounts const& counts, std::ostream* stream)
    {
        *stream << "{conflicts: " << counts.conflicts << ", failed: " << counts.failed << "}";
    }

    inline void PrintTo(Attention const& attention, std::ostream* stream)
    {
        *stream << "{conflicts: " << attention.counts.conflicts << ", failed: " << attention.counts.failed
                << ", pulse: " << std::boolalpha << attention.pulse << "}";
    }
}

namespace FileTracking::Test
{
    namespace AttentionStateTestDetail
    {
        inline AttentionCounts counts(std::uint64_t conflicts, std::uint64_t failed)
        {
            return AttentionCounts{.conflicts = conflicts, .failed = failed};
        }

        inline std::optional<Attention> shown(std::uint64_t conflicts, std::uint64_t failed, bool pulse)
        {
            return Attention{.counts = counts(conflicts, failed), .pulse = pulse};
        }
    }

    TEST(AttentionStateTests, StartsWithNothingSeenOrCounted)
    {
        const AttentionState state;

        EXPECT_FALSE(state.unseen());
        EXPECT_EQ(state.counts(), AttentionStateTestDetail::counts(0u, 0u));
    }

    TEST(AttentionStateTests, TheFirstConflictPulsesAndIsUnseen)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(1u, 0u), 1'000ms), shown(1u, 0u, true));
        EXPECT_TRUE(state.unseen());
        EXPECT_EQ(state.counts(), counts(1u, 0u));
    }

    TEST(AttentionStateTests, UnchangedCountsShowNothing)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(0u, 0u), 0ms), std::nullopt);
        EXPECT_EQ(state.update(counts(2u, 1u), 100ms), shown(2u, 1u, true));
        EXPECT_EQ(state.update(counts(2u, 1u), 200ms), std::nullopt);
        EXPECT_EQ(state.update(counts(2u, 1u), 10'000ms), std::nullopt);
    }

    TEST(AttentionStateTests, ConflictsWithinTheWindowOfAPulseDoNotPulseAgain)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(1u, 0u), 0ms), shown(1u, 0u, true));
        EXPECT_EQ(state.update(counts(2u, 0u), 1'000ms), shown(2u, 0u, false));
        EXPECT_EQ(state.update(counts(3u, 0u), 2'499ms), shown(3u, 0u, false));
        EXPECT_TRUE(state.unseen());
    }

    TEST(AttentionStateTests, AConflictOnceTheWindowEndedPulsesAgain)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(1u, 0u), 0ms), shown(1u, 0u, true));
        EXPECT_EQ(state.update(counts(2u, 0u), AttentionState::pulseWindow), shown(2u, 0u, true));
        EXPECT_EQ(state.update(counts(3u, 0u), 10'000ms), shown(3u, 0u, true));
    }

    TEST(AttentionStateTests, TheWindowStartsWithThePulseNotWithTheLastConflict)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(1u, 0u), 0ms), shown(1u, 0u, true));
        EXPECT_EQ(state.update(counts(2u, 0u), 2'000ms), shown(2u, 0u, false));
        EXPECT_EQ(state.update(counts(3u, 0u), 2'600ms), shown(3u, 0u, true));
        EXPECT_EQ(state.update(counts(4u, 0u), 5'000ms), shown(4u, 0u, false));
        EXPECT_EQ(state.update(counts(5u, 0u), 5'100ms), shown(5u, 0u, true));
    }

    TEST(AttentionStateTests, ABurstOfConflictsWhileAPulsePlaysPulsesOnce)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(1u, 0u), 0ms), shown(1u, 0u, true));
        auto pulses = 0;
        for (auto conflicts = 2u; conflicts <= 24u; ++conflicts)
        {
            const auto attention = state.update(counts(conflicts, 0u), std::chrono::milliseconds{conflicts * 100u});
            ASSERT_TRUE(attention.has_value());
            EXPECT_EQ(attention->counts, counts(conflicts, 0u));
            pulses += attention->pulse ? 1 : 0;
        }

        EXPECT_EQ(pulses, 0);
        EXPECT_EQ(state.update(counts(25u, 0u), 2'600ms), shown(25u, 0u, true));
    }

    TEST(AttentionStateTests, ConflictsAtTheSameMomentPulseOnce)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(1u, 0u), 500ms), shown(1u, 0u, true));
        EXPECT_EQ(state.update(counts(2u, 0u), 500ms), shown(2u, 0u, false));
        EXPECT_EQ(state.update(counts(3u, 0u), 500ms), shown(3u, 0u, false));
    }

    TEST(AttentionStateTests, FailuresNeverPulseNorMakeAnythingUnseen)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(0u, 1u), 0ms), shown(0u, 1u, false));
        EXPECT_EQ(state.update(counts(0u, 4u), 10'000ms), shown(0u, 4u, false));
        EXPECT_FALSE(state.unseen());
    }

    TEST(AttentionStateTests, FailuresDoNotStartTheWindow)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(0u, 1u), 0ms), shown(0u, 1u, false));
        EXPECT_EQ(state.update(counts(1u, 1u), 100ms), shown(1u, 1u, true));
    }

    TEST(AttentionStateTests, MoreFailuresBesideConflictsDoNotPulse)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(1u, 0u), 0ms), shown(1u, 0u, true));
        EXPECT_EQ(state.update(counts(1u, 1u), 5'000ms), shown(1u, 1u, false));
        EXPECT_EQ(state.update(counts(1u, 0u), 6'000ms), shown(1u, 0u, false));
        EXPECT_TRUE(state.unseen());
    }

    TEST(AttentionStateTests, FewerConflictsDoNotPulseNorMakeThemUnseen)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;

        EXPECT_EQ(state.update(counts(3u, 0u), 0ms), shown(3u, 0u, true));
        state.acknowledge();

        EXPECT_EQ(state.update(counts(2u, 0u), 5'000ms), shown(2u, 0u, false));
        EXPECT_FALSE(state.unseen());
    }

    TEST(AttentionStateTests, AcknowledgeMakesTheConflictsSoFarSeen)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;
        state.update(counts(2u, 1u), 0ms);
        state.acknowledge();

        EXPECT_FALSE(state.unseen());
        EXPECT_EQ(state.counts(), counts(2u, 1u));
        EXPECT_EQ(state.update(counts(2u, 1u), 100ms), std::nullopt);
        EXPECT_FALSE(state.unseen());
    }

    TEST(AttentionStateTests, AConflictAfterAcknowledgeIsUnseenAgain)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;
        state.update(counts(1u, 0u), 0ms);
        state.acknowledge();

        EXPECT_EQ(state.update(counts(2u, 0u), 1'000ms), shown(2u, 0u, false));
        EXPECT_TRUE(state.unseen());

        state.acknowledge();

        EXPECT_FALSE(state.unseen());
    }

    TEST(AttentionStateTests, NoConflictsLeftClearUnseen)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;
        state.update(counts(2u, 1u), 0ms);

        EXPECT_EQ(state.update(counts(0u, 1u), 100ms), shown(0u, 1u, false));
        EXPECT_FALSE(state.unseen());
    }

    TEST(AttentionStateTests, AConflictAfterAllWereAnsweredIsUnseenAndPulsesOnceTheWindowEnded)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;
        state.update(counts(1u, 0u), 0ms);
        state.update(counts(0u, 0u), 500ms);

        EXPECT_EQ(state.update(counts(1u, 0u), 1'000ms), shown(1u, 0u, false));
        EXPECT_TRUE(state.unseen());

        state.update(counts(0u, 0u), 2'000ms);

        EXPECT_EQ(state.update(counts(1u, 0u), 3'000ms), shown(1u, 0u, true));
        EXPECT_TRUE(state.unseen());
    }

    TEST(AttentionStateTests, ResetForgetsCountsUnseenAndTheLastPulse)
    {
        using namespace AttentionStateTestDetail;
        using namespace std::chrono_literals;
        AttentionState state;
        state.update(counts(2u, 3u), 0ms);
        state.reset();

        EXPECT_FALSE(state.unseen());
        EXPECT_EQ(state.counts(), counts(0u, 0u));
        EXPECT_EQ(state.update(counts(0u, 0u), 100ms), std::nullopt);
        EXPECT_EQ(state.update(counts(1u, 0u), 200ms), shown(1u, 0u, true));
        EXPECT_TRUE(state.unseen());
    }
}
