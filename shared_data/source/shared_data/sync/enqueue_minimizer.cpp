#include <shared_data/sync/enqueue_minimizer.hpp>

#include <algorithm>
#include <numeric>
#include <string_view>

namespace SharedData::Sync
{
    namespace
    {
        /**
         * @brief Calls @p visit with every proper ancestor of @p relKey, nearest first. Stops early when
         *        @p visit returns true.
         * @return True when @p visit returned true for an ancestor.
         */
        template <typename VisitorT>
        bool anyAncestor(std::string_view relKey, VisitorT&& visit)
        {
            while (true)
            {
                const auto slash = relKey.rfind('/');
                if (slash == std::string_view::npos)
                    return false;
                relKey = relKey.substr(0, slash);
                if (visit(relKey))
                    return true;
            }
        }
    }

    std::vector<std::size_t> minimizeEnqueueIndices(
        std::vector<MinimizerItemView> const& items,
        std::unordered_set<std::string> const& sparseSet
    )
    {
        const std::size_t count = items.size();
        if (count == 0)
            return {};

        // Ancestors are processed before their descendants, so a bulk emission can cover the subtree.
        std::vector<std::size_t> order(count);
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(
            order.begin(),
            order.end(),
            [&](std::size_t lhs, std::size_t rhs)
            {
                return items[lhs].relKey.size() < items[rhs].relKey.size();
            }
        );

        // Every proper ancestor of an item has a descendant, so it is no tree leaf.
        std::unordered_set<std::string_view> hasDescendant;
        hasDescendant.reserve(count);
        for (auto const& item : items)
        {
            anyAncestor(
                item.relKey,
                [&](std::string_view ancestor)
                {
                    return !hasDescendant.insert(ancestor).second;
                }
            );
        }

        const auto inSparseSet = [&](std::string_view relKey)
        {
            return sparseSet.contains(std::string{relKey});
        };

        std::unordered_set<std::string_view> bulkCovered;
        std::vector<std::size_t> result;
        result.reserve(count);

        for (const std::size_t index : order)
        {
            auto const& item = items[index];
            const std::string_view relKey{item.relKey};

            if (anyAncestor(
                    relKey,
                    [&](std::string_view ancestor)
                    {
                        return bulkCovered.contains(ancestor);
                    }
                ))
                continue;

            const bool effectivelySelected = sparseSet.contains(item.relKey) || anyAncestor(relKey, inSparseSet);
            if (!effectivelySelected)
                continue;

            if (item.isBulkDir)
            {
                result.push_back(index);
                bulkCovered.insert(relKey);
                continue;
            }

            // A two-sided directory has no single transfer; its leaves follow and inherit the selection.
            if (!hasDescendant.contains(relKey))
                result.push_back(index);
        }

        return result;
    }
}
