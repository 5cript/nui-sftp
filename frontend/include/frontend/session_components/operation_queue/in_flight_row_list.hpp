#pragma once

#include <frontend/session_components/file_tracking/in_flight_rows.hpp>

#include <nui/event_system/observed_value.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/**
 * @brief The observed rows of the files in flight, laid out by FileTracking::InFlightRows. Rows that are dropped leave
 *        the list before any row is shown: a row destroyed while its events are active is read by them after it is
 *        gone, so a dropped row must not be changed in the same update.
 */
template <typename RowT>
class InFlightRowList
{
  public:
    /**
     * @brief Takes the entries in flight now and shows every row that stays.
     *
     * @param entryIndices The entries in flight.
     * @param show Called as show(row, entryIndex) for every row, entryIndex is nothing for a free row.
     */
    template <typename ShowT>
    void update(std::vector<std::uint64_t> const& entryIndices, ShowT const& show)
    {
        layout_.update(entryIndices);
        auto const& slots = layout_.rows();
        auto const& rows = rows_.value();
        if (rows.size() > slots.size())
            rows_.erase(rows.begin() + static_cast<std::ptrdiff_t>(slots.size()), rows.end());

        std::vector<std::shared_ptr<RowT>> fresh{};
        for (std::size_t index = 0u; index < slots.size(); ++index)
        {
            if (index < rows_.value().size())
            {
                show(*rows_.value()[index], slots[index]);
                continue;
            }
            auto row = std::make_shared<RowT>();
            show(*row, slots[index]);
            fresh.push_back(std::move(row));
        }
        if (!fresh.empty())
            rows_.insert(rows_.value().end(), fresh.begin(), fresh.end());
    }

    /**
     * @brief Drops every row.
     */
    void clear()
    {
        layout_.clear();
        if (!rows_.value().empty())
            rows_.clear();
    }

    /**
     * @return The rows, for Nui::range.
     */
    Nui::Observed<std::vector<std::shared_ptr<RowT>>>& rows()
    {
        return rows_;
    }

  private:
    FileTracking::InFlightRows<std::uint64_t> layout_{};
    Nui::Observed<std::vector<std::shared_ptr<RowT>>> rows_{};
};
