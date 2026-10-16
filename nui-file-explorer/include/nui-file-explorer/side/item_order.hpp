#pragma once

#include <nui-file-explorer/item.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <algorithm>
#include <compare>
#include <iterator>
#include <vector>

namespace NuiFileExplorer
{
    enum class SortCriterion
    {
        Name,
        Size,
        Info,
        Mtime
    };

    /**
     * @brief How @p lhs orders against @p rhs by @p criterion, ascending. Names compare case-insensitively.
     */
    inline std::weak_ordering compareItems(Item const& lhs, Item const& rhs, SortCriterion criterion)
    {
        switch (criterion)
        {
            case SortCriterion::Name:
                return Utility::Algorithm::caseInsensitiveCompare(lhs.displayName(), rhs.displayName()) <=> 0;
            case SortCriterion::Size:
                return lhs.size <=> rhs.size;
            case SortCriterion::Info:
                return lhs.lsStyleTypePermsUserGroup() <=> rhs.lsStyleTypePermsUserGroup();
            case SortCriterion::Mtime:
                return lhs.mtime <=> rhs.mtime;
        }
        return std::weak_ordering::equivalent;
    }

    /**
     * @brief Sorts @p entries by @p criterion: the parent link first, then the directories, then everything else,
     *        each group in the order @p criterion and @p ascending give.
     *
     * @param itemOf Gives the @ref Item of an entry.
     */
    template <typename EntryT, typename ItemOfT>
    void sortItems(std::vector<EntryT>& entries, SortCriterion criterion, bool ascending, ItemOfT const& itemOf)
    {
        auto first = entries.begin();
        const auto parentLink = std::ranges::find_if(
            entries,
            [&itemOf](EntryT const& entry)
            {
                return itemOf(entry).isParentLink;
            }
        );
        if (parentLink != entries.end())
        {
            std::rotate(first, parentLink, std::next(parentLink));
            ++first;
        }

        const auto directoriesEnd = std::stable_partition(
            first,
            entries.end(),
            [&itemOf](EntryT const& entry)
            {
                return itemOf(entry).isDirectoryLike();
            }
        );
        const auto inOrder = [&itemOf, criterion, ascending](EntryT const& lhs, EntryT const& rhs)
        {
            const auto order = compareItems(itemOf(lhs), itemOf(rhs), criterion);
            return ascending ? order < 0 : order > 0;
        };
        std::sort(first, directoriesEnd, inOrder);
        std::sort(directoriesEnd, entries.end(), inOrder);
    }
}
