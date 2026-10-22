#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace FileTracking
{
    /**
     * @brief The keys of a table's rows in the order they were added, so the row of a key can be found.
     */
    template <typename KeyT>
    class RowOrder
    {
      public:
        /**
         * @brief Appends the row of @p key.
         */
        void add(KeyT key)
        {
            keys_.push_back(std::move(key));
        }

        /**
         * @brief Forgets the row of @p key.
         * @return The index the row had, nothing when @p key has no row.
         */
        std::optional<std::size_t> remove(KeyT const& key)
        {
            const auto found = std::ranges::find(keys_, key);
            if (found == keys_.end())
                return std::nullopt;
            const auto index = static_cast<std::size_t>(std::distance(keys_.begin(), found));
            keys_.erase(found);
            return index;
        }

        void clear()
        {
            keys_.clear();
        }

        std::size_t size() const
        {
            return keys_.size();
        }

      private:
        std::vector<KeyT> keys_{};
    };
}
