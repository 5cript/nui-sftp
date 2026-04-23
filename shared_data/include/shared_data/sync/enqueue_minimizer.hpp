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
     * @brief Computes the minimal set of indices to enqueue from @p items given a
     *        SPARSE selection set.
     *
     * Semantics of the sparse set @p sparseSet:
     *  - An entry X means "X and every descendant of X is selected" (ancestor
     *    implication).
     *  - Callers normally keep the set in its sparsest form by collapsing
     *    fully-selected sibling groups into the shared parent and filling out
     *    when a deeper descendant is unchecked.  The minimizer tolerates
     *    non-minimal input (redundant ancestors + descendants) — it just walks
     *    ancestor-in-set during processing.
     *
     * Emission rules:
     *  - Bulk-dir (one-sided directory or delete-dir) effectively selected →
     *    emit once and cover the whole subtree.
     *  - Plain file leaf effectively selected → emit.
     *  - Structural two-sided directory effectively selected → no own emission;
     *    descendants iterate next and inherit the selection.
     *
     * "Effectively selected" = in the set OR any ancestor is in the set.
     */
    std::vector<std::size_t> minimizeEnqueueIndices(
        std::vector<MinimizerItemView> const& items,
        std::unordered_set<std::string> const& sparseSet
    );
}
