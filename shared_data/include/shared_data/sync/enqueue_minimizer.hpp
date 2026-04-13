#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace SharedData::Sync
{
    /**
     * @brief Minimal projection of a diff item required by the enqueue minimizer.
     *
     * The minimizer is independent of UI types; callers convert their richer
     * diff/sync items into this shape before calling.
     */
    struct MinimizerItemView
    {
        std::string relKey;
        /// True when this item represents a one-shot bulk-directory operation (i.e. the
        /// missing-side absence + present-side directory pattern, or a directory delete).
        /// When true and every descendant of relKey is selected, the minimizer emits this
        /// item and skips its descendants.
        bool isBulkDir{false};
    };

    /**
     * @brief Computes the minimal set of indices to enqueue from @p items.
     *
     * Bulk-directory items collapse fully-selected subtrees into a single emission.
     * Partially-selected subtrees fall through to per-leaf emission. Non-leaf items
     * that are not bulk-directories are never emitted on their own (their leaves are).
     */
    std::vector<std::size_t> minimizeEnqueueIndices(
        std::vector<MinimizerItemView> const& items,
        std::unordered_set<std::string> const& selectedLeafRelKeys
    );
}
