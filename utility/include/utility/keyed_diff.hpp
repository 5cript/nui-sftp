#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Utility
{
    /**
     * @brief One step of turning a sequence into another, see keyedEdits.
     */
    struct KeyedEdit
    {
        enum class Kind
        {
            Erase,
            Insert,
            Update,
        };

        Kind kind{Kind::Erase};
        /** @brief Position in the sequence as left by the steps before this one. */
        std::size_t index{0};
        /** @brief Element of the target that is inserted or written; unused by Erase. */
        std::size_t source{0};

        bool operator==(KeyedEdit const&) const = default;
    };

    /**
     * @brief The erases, inserts and updates that turn @p current into @p target, in order.
     *
     * Elements are matched by key: an unchanged element costs no step, a changed one an Update in
     * place, a moved one an Erase and an Insert. Meant for observed lists, whose views then redraw
     * only the touched elements.
     *
     * @param key Maps an element to its key. Keys must be unique within each sequence.
     * @param equal Decides whether a matched element needs an Update.
     */
    template <typename T, typename KeyFunction, typename Equal = std::equal_to<T>>
    std::vector<KeyedEdit>
    keyedEdits(std::vector<T> const& current, std::vector<T> const& target, KeyFunction&& key, Equal equal = {})
    {
        using Key = std::decay_t<std::invoke_result_t<KeyFunction&, T const&>>;
        using Kind = KeyedEdit::Kind;

        std::unordered_map<Key, std::size_t> targetPositions{};
        targetPositions.reserve(target.size());
        for (std::size_t index = 0; index != target.size(); ++index)
            targetPositions.emplace(key(target[index]), index);

        std::vector<KeyedEdit> edits{};

        // The sequence as edited so far. Elements still from current remember where they came from,
        // inserted ones are taken from target and need no further update.
        struct Slot
        {
            Key key;
            std::optional<std::size_t> currentIndex;
        };
        std::vector<Slot> working{};
        working.reserve(current.size());

        // Back to front, so every erase index is still the index in current.
        for (std::size_t index = current.size(); index != 0; --index)
        {
            if (!targetPositions.contains(key(current[index - 1])))
                edits.push_back({.kind = Kind::Erase, .index = index - 1});
        }
        for (std::size_t index = 0; index != current.size(); ++index)
        {
            auto elementKey = key(current[index]);
            if (targetPositions.contains(elementKey))
                working.push_back(Slot{.key = std::move(elementKey), .currentIndex = index});
        }

        std::size_t position = 0;
        while (position < target.size())
        {
            const auto& wanted = key(target[position]);
            if (position < working.size() && working[position].key == wanted)
            {
                const auto& origin = working[position].currentIndex;
                if (origin && !equal(current[*origin], target[position]))
                    edits.push_back({.kind = Kind::Update, .index = position, .source = position});
                ++position;
                continue;
            }

            if (position < working.size())
            {
                const auto found = std::find_if(
                    working.begin() + static_cast<std::ptrdiff_t>(position),
                    working.end(),
                    [&wanted](Slot const& slot) {
                        return slot.key == wanted;
                    }
                );
                if (found != working.end())
                {
                    const auto from = static_cast<std::size_t>(std::distance(working.begin(), found));
                    const auto targetOfDisplaced = targetPositions.at(working[position].key);
                    // Move whichever element travels farther. One element moved down is then taken
                    // out and put back, instead of every element it passed being moved up.
                    if (targetOfDisplaced - position > from - position)
                    {
                        edits.push_back({.kind = Kind::Erase, .index = position});
                        working.erase(working.begin() + static_cast<std::ptrdiff_t>(position));
                        continue;
                    }
                    edits.push_back({.kind = Kind::Erase, .index = from});
                    working.erase(found);
                }
            }

            edits.push_back({.kind = Kind::Insert, .index = position, .source = position});
            working.insert(
                working.begin() + static_cast<std::ptrdiff_t>(position),
                Slot{.key = wanted, .currentIndex = std::nullopt}
            );
            ++position;
        }

        // Only reached with duplicate keys, which break the contract; still end with the target.
        while (working.size() > target.size())
        {
            edits.push_back({.kind = Kind::Erase, .index = target.size()});
            working.erase(working.begin() + static_cast<std::ptrdiff_t>(target.size()));
        }
        return edits;
    }
}
