#pragma once

#include <ids/ids.hpp>
#include <utility/describe.hpp>

#include <array>
#include <cstddef>
#include <variant>

namespace FileTracking
{
    /**
     * @brief The groups of the outliner, in the order they are shown. Their names are the values of data-ft-group.
     */
    BOOST_DEFINE_ENUM_CLASS(OutlinerGroup, NeedsYou, Syncing, Watching, Stopped, Orphaned)

    /**
     * @brief Every outliner group in display order.
     */
    inline constexpr std::array<OutlinerGroup, 5> outlinerGroupOrder{
        OutlinerGroup::NeedsYou,
        OutlinerGroup::Syncing,
        OutlinerGroup::Watching,
        OutlinerGroup::Stopped,
        OutlinerGroup::Orphaned,
    };

    /**
     * @brief What a watch is doing, shown by its glyph and status badge. Its names are the values of data-ft-state and
     *        data-ft-status. Conflicts wins over Failed when a watch has both.
     */
    BOOST_DEFINE_ENUM_CLASS(WatchStatus, Watching, Syncing, Conflicts, Failed, Stopped)

    /**
     * @brief The tabs of the watch detail, in the order they are shown. Their names are the data-tab-key values.
     */
    BOOST_DEFINE_ENUM_CLASS(DetailTab, Conflicts, Failed, Options)

    /**
     * @brief Every detail tab in display order.
     */
    inline constexpr std::array<DetailTab, 3> detailTabOrder{
        DetailTab::Conflicts,
        DetailTab::Failed,
        DetailTab::Options,
    };

    /**
     * @brief How many conflicts or failures one page of the Conflicts and Failed tabs shows.
     */
    inline constexpr std::size_t problemPageSize = 100u;

    /**
     * @brief How Up, Down, Home and End move the selection through the expanded outliner groups.
     */
    enum class SelectionMove
    {
        Previous,
        Next,
        First,
        Last,
    };

    /**
     * @brief What the detail pane shows: nothing, a watch, or an orphaned copy.
     */
    using Selection = std::variant<std::monostate, Ids::WatchId, Ids::InstanceId>;
}
