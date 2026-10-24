#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace FileTracking
{
    /**
     * @brief The conflicts or the failures of one watch, newest first: a show of a new id goes to the front, a show of
     *        a known id replaces its value and moves it to the front. Lookups by id and the size are cheap, a page is a
     *        slice by offset. Pure, tested natively; TrackingModel keeps one per watch and kind.
     */
    template <typename IdT, typename ValueT>
    class ProblemList
    {
      public:
        /**
         * @brief One problem with the sequence of its last show.
         */
        struct ListedProblem
        {
            std::uint64_t sequence;
            IdT id;
            ValueT value;
        };

        using ConstIterator = typename std::deque<ListedProblem>::const_iterator;

        /**
         * @brief Shows @p value under @p id, replacing what @p id held and moving it to its place by @p sequence.
         * @param sequence Greater is newer. Distinct per show; the newest show passes the greatest and lands in front.
         * @return Whether @p id was new.
         */
        bool show(IdT const& id, ValueT value, std::uint64_t sequence)
        {
            const auto [known, inserted] = sequences_.try_emplace(id, sequence);
            if (!inserted)
            {
                problems_.erase(positionOf(known->second));
                known->second = sequence;
            }
            problems_.insert(
                positionOf(sequence), ListedProblem{.sequence = sequence, .id = id, .value = std::move(value)}
            );
            return inserted;
        }

        /**
         * @brief Forgets @p id.
         * @return Whether it was known.
         */
        bool remove(IdT const& id)
        {
            const auto known = sequences_.find(id);
            if (known == sequences_.end())
                return false;
            problems_.erase(positionOf(known->second));
            sequences_.erase(known);
            return true;
        }

        /**
         * @brief Forgets every one of @p ids in one pass over the list; unknown and repeated ids are skipped.
         * @return How many were known.
         */
        std::size_t remove(std::vector<IdT> const& ids)
        {
            std::vector<std::uint64_t> removed{};
            removed.reserve(ids.size());
            for (auto const& id : ids)
            {
                if (const auto known = sequences_.find(id); known != sequences_.end())
                {
                    removed.push_back(known->second);
                    sequences_.erase(known);
                }
            }
            if (removed.size() == 1u)
                problems_.erase(positionOf(removed.front()));
            else if (!removed.empty())
            {
                std::ranges::sort(removed);
                std::erase_if(
                    problems_,
                    [&removed](ListedProblem const& problem)
                    {
                        return std::ranges::binary_search(removed, problem.sequence);
                    }
                );
            }
            return removed.size();
        }

        /**
         * @brief Forgets every problem.
         */
        void clear()
        {
            problems_.clear();
            sequences_.clear();
        }

        /**
         * @return The value of @p id, nullptr when it is not known.
         */
        ValueT const* find(IdT const& id) const
        {
            const auto rank = rankOf(id);
            return rank ? &problems_[*rank].value : nullptr;
        }

        /**
         * @return Whether @p id is known.
         */
        bool contains(IdT const& id) const
        {
            return sequences_.contains(id);
        }

        /**
         * @return Where @p id is listed, 0 for the newest, nothing when it is not known.
         */
        std::optional<std::size_t> rankOf(IdT const& id) const
        {
            const auto known = sequences_.find(id);
            if (known == sequences_.end())
                return std::nullopt;
            return static_cast<std::size_t>(std::distance(problems_.begin(), positionOf(known->second)));
        }

        /**
         * @return The values from rank @p offset on, at most @p count of them, newest first.
         */
        std::vector<ValueT> slice(std::size_t offset, std::size_t count) const
        {
            if (offset >= problems_.size())
                return {};
            const auto end = offset + std::min(count, problems_.size() - offset);
            std::vector<ValueT> values{};
            values.reserve(end - offset);
            for (auto rank = offset; rank < end; ++rank)
                values.push_back(problems_[rank].value);
            return values;
        }

        /**
         * @return The newest problem, nullptr while the list is empty.
         */
        ListedProblem const* newest() const
        {
            return problems_.empty() ? nullptr : &problems_.front();
        }

        /**
         * @return The number of problems.
         */
        std::size_t size() const
        {
            return problems_.size();
        }

        /**
         * @return Whether there is no problem.
         */
        bool empty() const
        {
            return problems_.empty();
        }

        /**
         * @return The first problem, the newest, for iterating newest first.
         */
        ConstIterator begin() const
        {
            return problems_.cbegin();
        }

        /**
         * @return Past the oldest problem.
         */
        ConstIterator end() const
        {
            return problems_.cend();
        }

      private:
        /**
         * @return The first problem not newer than @p sequence: the one shown with it, or where it goes.
         */
        auto positionOf(std::uint64_t sequence) const
        {
            return std::ranges::lower_bound(problems_, sequence, std::greater<>{}, &ListedProblem::sequence);
        }

      private:
        /** @brief Sorted by descending sequence. */
        std::deque<ListedProblem> problems_{};
        std::map<IdT, std::uint64_t> sequences_{};
    };
}
