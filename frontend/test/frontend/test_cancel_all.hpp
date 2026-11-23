#pragma once

#include <frontend/session_components/operation_queue/cancel_all.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace CancelAll::Test
{
    namespace CancelAllTestDetail
    {
        using SharedData::PartialFilesOnCancel;

        /**
         * @brief A transfer that is not ended, with "Delete Partial Files" on.
         */
        inline Card running(std::string const& name)
        {
            return Card{.operationId = Ids::makeOperationId(name), .keepsWhenAsked = true};
        }

        /**
         * @brief A transfer that is not ended, with "Delete Partial Files" off.
         */
        inline Card runningWithoutCleanup(std::string const& name)
        {
            return Card{.operationId = Ids::makeOperationId(name), .keepsWhenAsked = true, .keepsAsConfigured = true};
        }

        /**
         * @brief An archive transfer that is not ended, nothing continues its partial archive.
         */
        inline Card runningArchive(std::string const& name)
        {
            return Card{.operationId = Ids::makeOperationId(name)};
        }

        /**
         * @brief An ended card without partial files: finished, failed, or canceled deleting them.
         */
        inline Card ended(std::string const& name)
        {
            return Card{.operationId = Ids::makeOperationId(name), .ended = true, .keepsWhenAsked = true};
        }

        /**
         * @brief A card canceled keeping its partial files.
         */
        inline Card kept(std::string const& name)
        {
            return Card{
                .operationId = Ids::makeOperationId(name),
                .ended = true,
                .keptPartialFiles = true,
                .keepsWhenAsked = true,
            };
        }

        inline std::vector<std::string> names(std::vector<Ids::OperationId> const& operationIds)
        {
            std::vector<std::string> result;
            for (auto const& operationId : operationIds)
                result.push_back(operationId.value());
            return result;
        }

        /**
         * @brief Each canceled card by name, with "+kept" appended when it keeps its partial files.
         */
        inline std::vector<std::string> names(std::vector<Canceled> const& canceled)
        {
            std::vector<std::string> result;
            for (auto const& card : canceled)
                result.push_back(card.keepsPartialFiles ? card.operationId.value() + "+kept" : card.operationId.value());
            return result;
        }

        using Names = std::vector<std::string>;
    }

    TEST(CancelAllTests, TheCardsBeforeAreSortedByHowFarTheyAre)
    {
        using namespace CancelAllTestDetail;
        const auto before = cardsBefore({running("r"), ended("e"), kept("k")});
        EXPECT_EQ(before.active, (OperationIds{Ids::makeOperationId("r")}));
        EXPECT_EQ(before.ended, (OperationIds{Ids::makeOperationId("e"), Ids::makeOperationId("k")}));
        EXPECT_EQ(before.kept, (OperationIds{Ids::makeOperationId("k")}));
    }

    TEST(CancelAllTests, APlainCancelAllLeavesWhatItCancelsAndForgetsTheKeptCardsItRemoves)
    {
        using namespace CancelAllTestDetail;
        const std::vector<Card> cards{running("r"), ended("e"), kept("k")};
        const auto result = outcome(PartialFilesOnCancel::AsConfigured, cardsBefore(cards), cards);
        EXPECT_EQ(names(result.canceled), (Names{"r"}));
        EXPECT_EQ(names(result.removed), (Names{"e", "k"}));
        EXPECT_EQ(names(result.forgotten), (Names{"k"}));
        EXPECT_EQ(names(result.discarded), (Names{}));
    }

    TEST(CancelAllTests, APlainCancelAllKeepsThePartialFilesOfATransferWithoutCleanup)
    {
        using namespace CancelAllTestDetail;
        const std::vector<Card> cards{running("r"), runningWithoutCleanup("w")};
        const auto result = outcome(PartialFilesOnCancel::AsConfigured, cardsBefore(cards), cards);
        EXPECT_EQ(names(result.canceled), (Names{"r", "w+kept"}));
    }

    TEST(CancelAllTests, DeletingPartialFilesDiscardsTheKeptOnesAndKeepsNone)
    {
        using namespace CancelAllTestDetail;
        const std::vector<Card> cards{running("r"), runningWithoutCleanup("w"), ended("e"), kept("k")};
        const auto result = outcome(PartialFilesOnCancel::Deleted, cardsBefore(cards), cards);
        EXPECT_EQ(names(result.canceled), (Names{"r", "w"}));
        EXPECT_EQ(names(result.removed), (Names{"e", "k"}));
        EXPECT_EQ(names(result.forgotten), (Names{}));
        EXPECT_EQ(names(result.discarded), (Names{"k"}));
    }

    TEST(CancelAllTests, KeepingPartialFilesKeepsThoseOfWhatItCancelsExceptOfAnArchive)
    {
        using namespace CancelAllTestDetail;
        const std::vector<Card> cards{running("r"), runningArchive("a"), ended("e"), kept("k")};
        const auto result = outcome(PartialFilesOnCancel::Kept, cardsBefore(cards), cards);
        EXPECT_EQ(names(result.canceled), (Names{"r+kept", "a"}));
        EXPECT_EQ(names(result.removed), (Names{"e", "k"}));
        EXPECT_EQ(names(result.forgotten), (Names{"k"}));
        EXPECT_EQ(names(result.discarded), (Names{}));
    }

    TEST(CancelAllTests, ACardThatEndedBeforeTheReplyStaysAsItsCompletionLeftIt)
    {
        using namespace CancelAllTestDetail;
        const auto before = cardsBefore({running("r"), running("s")});
        for (const auto partialFiles :
             {PartialFilesOnCancel::AsConfigured, PartialFilesOnCancel::Deleted, PartialFilesOnCancel::Kept})
        {
            const auto result = outcome(partialFiles, before, {kept("r"), ended("s")});
            EXPECT_EQ(names(result.canceled), (Names{}));
            EXPECT_EQ(names(result.removed), (Names{}));
            EXPECT_EQ(names(result.forgotten), (Names{}));
            EXPECT_EQ(names(result.discarded), (Names{}));
        }
    }

    TEST(CancelAllTests, AContinuationThatTookThePlaceOfAKeptCardStaysWithItsPartialFiles)
    {
        using namespace CancelAllTestDetail;
        const auto before = cardsBefore({kept("k")});
        for (const auto partialFiles :
             {PartialFilesOnCancel::AsConfigured, PartialFilesOnCancel::Deleted, PartialFilesOnCancel::Kept})
        {
            const auto result = outcome(partialFiles, before, {running("k")});
            EXPECT_EQ(names(result.canceled), (Names{}));
            EXPECT_EQ(names(result.removed), (Names{}));
            EXPECT_EQ(names(result.forgotten), (Names{}));
            EXPECT_EQ(names(result.discarded), (Names{}));
        }
    }

    TEST(CancelAllTests, ACardAddedAfterTheCancelAllIsLeftAlone)
    {
        using namespace CancelAllTestDetail;
        const auto before = cardsBefore({running("r")});
        const auto result = outcome(PartialFilesOnCancel::AsConfigured, before, {running("r"), running("n"), ended("m")});
        EXPECT_EQ(names(result.canceled), (Names{"r"}));
        EXPECT_EQ(names(result.removed), (Names{}));
    }

    TEST(CancelAllTests, AKeptCardThatWentBeforeTheReplyIsNeitherForgottenNorDiscarded)
    {
        using namespace CancelAllTestDetail;
        const auto before = cardsBefore({kept("k")});
        for (const auto partialFiles : {PartialFilesOnCancel::AsConfigured, PartialFilesOnCancel::Deleted})
        {
            const auto result = outcome(partialFiles, before, {});
            EXPECT_EQ(names(result.removed), (Names{}));
            EXPECT_EQ(names(result.forgotten), (Names{}));
            EXPECT_EQ(names(result.discarded), (Names{}));
        }
    }
}
