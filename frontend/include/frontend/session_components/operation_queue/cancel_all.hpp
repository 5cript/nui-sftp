#pragma once

#include <ids/ids.hpp>
#include <shared_data/file_operations/partial_files_on_cancel.hpp>

#include <unordered_set>
#include <vector>

/**
 * @brief What a Cancel All of the operation queue does with its cards, see docs/cancel-behavior.md.
 */
namespace CancelAll
{
    using OperationIds = std::unordered_set<Ids::OperationId, Ids::IdHash>;

    /**
     * @brief The ways to cancel all: the button and the two entries of its menu.
     */
    enum class Variant
    {
        /** @brief Deletes the partial files of the transfers it cancels, those that cards kept before stay. */
        Plain,
        /** @brief Deletes the partial files of the transfers it cancels and those that cards kept before. */
        DeletingPartialFiles,
        /** @brief Keeps the partial files of the transfers it cancels and those that cards kept before. */
        KeepingPartialFiles,
    };

    /**
     * @brief What the backend does with the partial files of the transfers a Cancel All of @p variant cancels.
     */
    inline SharedData::PartialFilesOnCancel partialFilesOnCancel(Variant variant)
    {
        return variant == Variant::KeepingPartialFiles ? SharedData::PartialFilesOnCancel::Kept
                                                       : SharedData::PartialFilesOnCancel::Deleted;
    }

    /**
     * @brief What a Cancel All needs to know of a card.
     */
    struct Card
    {
        Ids::OperationId operationId{};
        /** @brief Completed, failed or canceled. */
        bool ended{false};
        /** @brief Canceled keeping its partial files. */
        bool keptPartialFiles{false};
        /** @brief A cancel that is asked to keep the partial files keeps them, an archive never does. */
        bool keepsWhenAsked{false};
    };

    /**
     * @brief The cards when the Cancel All was asked for. The completions of its cancels may come before its reply,
     *        so the reply cannot tell anymore which cards had ended before.
     */
    struct CardsBefore
    {
        /** @brief Not ended yet. */
        OperationIds active{};
        /** @brief Ended, the ones that kept their partial files included. */
        OperationIds ended{};
        /** @brief Canceled keeping their partial files. */
        OperationIds kept{};
    };

    /**
     * @brief A card to show as canceled until its completion says the same.
     */
    struct Canceled
    {
        Ids::OperationId operationId{};
        bool keepsPartialFiles{false};
    };

    /**
     * @brief What the reply to a Cancel All does, each list in the order of the cards.
     */
    struct Outcome
    {
        /** @brief The cards that go, they had ended before. */
        std::vector<Ids::OperationId> removed{};
        /** @brief The cards of the operations it canceled, they stay. */
        std::vector<Canceled> canceled{};
        /** @brief Removed cards whose kept partial files stay, nothing continues or discards them anymore. */
        std::vector<Ids::OperationId> forgotten{};
        /** @brief Removed cards whose kept partial files are deleted. */
        std::vector<Ids::OperationId> discarded{};
    };

    inline CardsBefore cardsBefore(std::vector<Card> const& cards)
    {
        CardsBefore before{};
        for (auto const& card : cards)
        {
            if (!card.ended)
            {
                before.active.insert(card.operationId);
                continue;
            }
            before.ended.insert(card.operationId);
            if (card.keptPartialFiles)
                before.kept.insert(card.operationId);
        }
        return before;
    }

    /**
     * @brief Whether the transfer of @p card keeps its partial files when a Cancel All of @p variant cancels it.
     */
    inline bool keepsPartialFiles(Variant variant, Card const& card)
    {
        return variant == Variant::KeepingPartialFiles && card.keepsWhenAsked;
    }

    /**
     * @brief What the reply to a Cancel All of @p variant does with the cards @p now, given the cards @p before it. A
     *        card counts as it is now: one that ended meanwhile was canceled by this and stays, and a continuation
     *        that took the place of a kept card is no ended card anymore.
     */
    inline Outcome outcome(Variant variant, CardsBefore const& before, std::vector<Card> const& now)
    {
        Outcome result{};
        for (auto const& card : now)
        {
            if (!card.ended)
            {
                if (before.active.contains(card.operationId))
                    result.canceled.push_back({card.operationId, keepsPartialFiles(variant, card)});
                continue;
            }
            if (!before.ended.contains(card.operationId))
                continue;
            result.removed.push_back(card.operationId);
            if (!card.keptPartialFiles || !before.kept.contains(card.operationId))
                continue;
            if (variant == Variant::DeletingPartialFiles)
                result.discarded.push_back(card.operationId);
            else
                result.forgotten.push_back(card.operationId);
        }
        return result;
    }
}
