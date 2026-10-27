#pragma once

#include <frontend/file_explorer/overwrite_decisions.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace FileExplorer::Test
{
    namespace OverwriteDecisionsTestDetail
    {
        struct Entry
        {
            std::string name{};
            bool mayOverwrite{false};
        };

        inline std::vector<Entry> entries(std::vector<std::string> const& names)
        {
            std::vector<Entry> result;
            for (auto const& name : names)
                result.push_back(Entry{.name = name});
            return result;
        }

        /**
         * @brief Each accepted name with "!" appended when it may overwrite, "cancelled" alone when cancelled.
         */
        inline std::vector<std::string> describe(std::optional<std::vector<Entry>> const& accepted)
        {
            std::vector<std::string> result;
            if (!accepted)
                return {"cancelled"};
            for (auto const& entry : *accepted)
                result.push_back(entry.mayOverwrite ? entry.name + "!" : entry.name);
            return result;
        }
    }

    TEST(OverwriteDecisionsTests, AbsentDestinationsAreAcceptedWithoutConsentToOverwrite)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b"}), {false, false}};
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"a", "b"}));
    }

    TEST(OverwriteDecisionsTests, AnExistingDestinationIsAskedAbout)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b", "c"}), {false, true, false}};
        const auto* question = decisions.nextQuestion();
        ASSERT_NE(question, nullptr);
        EXPECT_EQ(question->name, "b");
        decisions.answer(OverwriteAnswer::Yes);
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"a", "b!", "c"}));
    }

    TEST(OverwriteDecisionsTests, NoSkipsOnlyThatCandidate)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b"}), {true, true}};
        ASSERT_NE(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::No);
        const auto* question = decisions.nextQuestion();
        ASSERT_NE(question, nullptr);
        EXPECT_EQ(question->name, "b");
        decisions.answer(OverwriteAnswer::Yes);
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"b!"}));
    }

    TEST(OverwriteDecisionsTests, AllOverwritesEveryLaterExistingDestinationOnly)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b", "c", "d"}), {true, false, true, false}};
        ASSERT_NE(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::All);
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"a!", "b", "c!", "d"}));
    }

    TEST(OverwriteDecisionsTests, NoneSkipsEveryLaterExistingDestination)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b", "c"}), {true, false, true}};
        ASSERT_NE(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::None);
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"b"}));
    }

    TEST(OverwriteDecisionsTests, CancelDropsWhatWasApprovedBefore)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b", "c", "d"}), {false, true, true, false}};
        ASSERT_NE(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::Yes);
        ASSERT_NE(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::Cancel);
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"cancelled"}));
    }

    TEST(OverwriteDecisionsTests, CancelAtTheFirstQuestionTransfersNothing)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b"}), {true, false}};
        ASSERT_NE(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::Cancel);
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"cancelled"}));
    }

    TEST(OverwriteDecisionsTests, AnAnswerAfterCancelChangesNothing)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b"}), {true, true}};
        ASSERT_NE(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::Cancel);
        decisions.answer(OverwriteAnswer::All);
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"cancelled"}));
    }

    TEST(OverwriteDecisionsTests, AMissingAnswerCountsAsExisting)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a", "b"}), {false}};
        const auto* question = decisions.nextQuestion();
        ASSERT_NE(question, nullptr);
        EXPECT_EQ(question->name, "b");
    }

    TEST(OverwriteDecisionsTests, AnAnswerWithoutQuestionChangesNothing)
    {
        using namespace OverwriteDecisionsTestDetail;
        OverwriteDecisions decisions{entries({"a"}), {false}};
        EXPECT_EQ(decisions.nextQuestion(), nullptr);
        decisions.answer(OverwriteAnswer::Yes);
        EXPECT_EQ(describe(decisions.takeAccepted()), (std::vector<std::string>{"a"}));
    }

    TEST(OverwriteDecisionsTests, ApprovedForOverwriteSetsTheFlagOnACopy)
    {
        using namespace OverwriteDecisionsTestDetail;
        const Entry entry{.name = "x"};
        EXPECT_TRUE(approvedForOverwrite(entry).mayOverwrite);
        EXPECT_FALSE(entry.mayOverwrite);
    }
}
