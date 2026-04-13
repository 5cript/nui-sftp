#include <shared_data/sync/enqueue_minimizer.hpp>

#include <algorithm>
#include <numeric>

namespace SharedData::Sync
{
    std::vector<std::size_t> minimizeEnqueueIndices(
        std::vector<MinimizerItemView> const& items,
        std::unordered_set<std::string> const& selectedLeafRelKeys
    )
    {
        const std::size_t count = items.size();
        if (count == 0)
            return {};

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

        // For a directory-role item the "any" / "all selected" questions reduce to
        // scanning its descendant tree-leaves.
        const auto subtreeSelectionState = [&](std::size_t idx) {
            struct State
            {
                bool any{false};
                bool all{true};
            } state;
            if (isTreeLeaf[idx])
            {
                const bool checked = selectedLeafRelKeys.contains(items[idx].relKey);
                state.any = checked;
                state.all = checked;
                return state;
            }
            const std::string prefix = items[idx].relKey + "/";
            bool sawAnyLeaf = false;
            for (std::size_t other = 0; other < count; ++other)
            {
                if (!isTreeLeaf[other])
                    continue;
                if (items[other].relKey.size() <= prefix.size() ||
                    !items[other].relKey.starts_with(prefix))
                    continue;
                sawAnyLeaf = true;
                const bool checked = selectedLeafRelKeys.contains(items[other].relKey);
                state.any = state.any || checked;
                if (!checked)
                    state.all = false;
            }
            if (!sawAnyLeaf)
                state.all = false;
            return state;
        };

        std::vector<std::size_t> order(count);
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
            return items[lhs].relKey.size() < items[rhs].relKey.size();
        });

        std::vector<std::string> coveredPrefixes;
        std::vector<std::size_t> result;
        result.reserve(count);

        const auto isCovered = [&](std::string const& relKey) {
            for (auto const& ancestor : coveredPrefixes)
            {
                const std::string prefix = ancestor + "/";
                if (relKey.size() > prefix.size() && relKey.starts_with(prefix))
                    return true;
            }
            return false;
        };

        for (std::size_t idx : order)
        {
            auto const& item = items[idx];
            if (isCovered(item.relKey))
                continue;

            if (item.isBulkDir)
            {
                const auto state = subtreeSelectionState(idx);
                if (!state.any)
                    continue;
                if (state.all)
                {
                    result.push_back(idx);
                    coveredPrefixes.push_back(item.relKey);
                    continue;
                }
                continue;
            }

            if (!isTreeLeaf[idx])
                continue;
            if (!selectedLeafRelKeys.contains(item.relKey))
                continue;

            result.push_back(idx);
        }

        return result;
    }
}
