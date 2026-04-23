#include <shared_data/sync/enqueue_minimizer.hpp>

#include <algorithm>
#include <numeric>

namespace SharedData::Sync
{
    namespace
    {
        /** @brief Walks every parent prefix of @p relKey (excluding @p relKey
         *         itself) and returns true when any of them is present in
         *         @p sparseSet.  "Effective selection" in the sparse model:
         *         an entry X in the set implies every descendant of X is
         *         selected too.
         */
        bool anyAncestorInSet(
            std::string const& relKey,
            std::unordered_set<std::string> const& sparseSet
        )
        {
            std::string_view view{relKey};
            while (true)
            {
                const auto slash = view.rfind('/');
                if (slash == std::string_view::npos)
                    return false;
                view.remove_suffix(view.size() - slash);
                if (sparseSet.contains(std::string{view}))
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

        // Precompute "tree-leaf" flag — true when no other item is a strict
        // descendant.  For the sparse minimizer this only matters for pretty
        // diagnostic reasons; the emission rules below don't actually use it.
        // Kept around in case a future callers want it.

        // Process in ascending relKey length so ancestors are handled before
        // their descendants.  That lets bulk-dir coverage short-circuit the
        // descendants, and structural-dir membership flows down naturally via
        // anyAncestorInSet.
        std::vector<std::size_t> order(count);
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
            return items[lhs].relKey.size() < items[rhs].relKey.size();
        });

        std::vector<std::string> bulkCoveredPrefixes;
        std::vector<std::size_t> result;
        result.reserve(count);

        const auto isBulkCovered = [&](std::string const& relKey) {
            for (auto const& ancestor : bulkCoveredPrefixes)
            {
                const std::string prefix = ancestor + "/";
                if (relKey.size() > prefix.size() && relKey.starts_with(prefix))
                    return true;
            }
            return false;
        };

        // Precompute tree-leaf for disambiguating structural vs file rows:
        // a row with isBulkDir=false and any descendant in items is structural;
        // with no descendants it's a plain file.
        std::vector<bool> isTreeLeaf(count, true);
        for (std::size_t idx = 0; idx < count; ++idx)
        {
            const std::string prefix = items[idx].relKey + "/";
            for (std::size_t other = 0; other < count; ++other)
            {
                if (other == idx)
                    continue;
                if (items[other].relKey.size() > prefix.size() &&
                    items[other].relKey.starts_with(prefix))
                {
                    isTreeLeaf[idx] = false;
                    break;
                }
            }
        }

        for (std::size_t idx : order)
        {
            auto const& item = items[idx];

            // An ancestor was already emitted as a bulk op — everything below
            // it is covered by that single enqueue entry.
            if (isBulkCovered(item.relKey))
                continue;

            const bool inSet = sparseSet.contains(item.relKey);
            const bool ancestorInSet = anyAncestorInSet(item.relKey, sparseSet);
            const bool effectivelySelected = inSet || ancestorInSet;
            if (!effectivelySelected)
                continue;

            if (item.isBulkDir)
            {
                // One-sided directory or delete-directory — collapses the
                // entire subtree into a single enqueue entry regardless of
                // whether the item is in the set itself or just inherits.
                result.push_back(idx);
                bulkCoveredPrefixes.push_back(item.relKey);
                continue;
            }

            if (isTreeLeaf[idx])
            {
                // Plain file leaf (no descendants, not bulk).
                result.push_back(idx);
                continue;
            }

            // Structural two-sided directory.  No SFTP primitive syncs it as a
            // single op, so don't emit the row itself; the descendant leaves
            // iterate next and inherit the selection via anyAncestorInSet.
        }

        return result;
    }
}
