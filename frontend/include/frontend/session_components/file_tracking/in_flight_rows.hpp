#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace FileTracking
{
    /**
     * @brief Hands the files in flight out to a list of rows so nothing shifts while they move: a key keeps its row
     *        while it stays in flight, a new key takes the first free row and appends one when all are taken, and free
     *        rows at the end are dropped. Generalised from DisplayedBulkOperation's in-flight rows. Pure, tested
     *        natively; the store mirrors the rows onto observed rows of the progress block.
     */
    template <typename KeyT>
    class InFlightRows
    {
      public:
        /**
         * @brief Takes the keys in flight now: rows whose key is not among @p keys become free, then every key of
         *        @p keys without a row claims one, then free rows at the end are dropped.
         */
        void update([[maybe_unused]] std::vector<KeyT> const& keys)
        {}

        /**
         * @return The key of each row, nothing for a free row. Never ends with a free row.
         */
        std::vector<std::optional<KeyT>> const& rows() const
        {
            return rows_;
        }

        /**
         * @return The row of @p key, nothing when it is not in flight.
         */
        std::optional<std::size_t> rowOf([[maybe_unused]] KeyT const& key) const
        {
            return std::nullopt;
        }

        /**
         * @return The number of rows, free ones in between included.
         */
        std::size_t size() const
        {
            return rows_.size();
        }

        /**
         * @brief Drops every row.
         */
        void clear()
        {
            rows_.clear();
        }

      private:
        std::vector<std::optional<KeyT>> rows_{};
    };
}
