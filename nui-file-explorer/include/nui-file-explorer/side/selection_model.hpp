#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <utility>

namespace NuiFileExplorer
{
    /**
     * @brief The explorer selection by item index, in two layers.
     *
     * The range layer is the contiguous shift selection from the anchor to the flag (both inclusive, in either
     * order); without a flag it is the anchor alone. The mask layer is applied on top of it: indices in the added set
     * are selected regardless of the range, indices in the removed set are punched out of it.
     *
     * Selected = (range - removed) + added. Which indices may be selected at all (hidden by a filter, the ".." entry)
     * is decided by the caller, see @ref selectedIndices.
     */
    class SelectionModel
    {
      public:
        /**
         * @brief Forgets every selection.
         */
        void clear()
        {
            anchor_.reset();
            flag_.reset();
            clearMask();
        }

        /**
         * @brief Forgets the added and removed indices, the range stays.
         */
        void clearMask()
        {
            added_.clear();
            removed_.clear();
        }

        /**
         * @brief Selects only @p index, as a plain click does.
         */
        void collapseTo(std::size_t index)
        {
            clearMask();
            flag_.reset();
            anchor_ = index;
        }

        /**
         * @brief Selects the range of all @p count indices, nothing when @p count is 0.
         */
        void selectAll(std::size_t count)
        {
            clearMask();
            flag_.reset();
            anchor_.reset();
            if (count == 0)
                return;
            anchor_ = 0;
            flag_ = count - 1;
        }

        /**
         * @brief Selects the range from @p begin to @p endInclusive only.
         */
        void replaceRange(std::size_t begin, std::size_t endInclusive)
        {
            clearMask();
            anchor_ = begin;
            flag_ = endInclusive;
        }

        /**
         * @brief Moves the flag to @p index, the anchor stays and is set to @p index when there is none.
         */
        void extendTo(std::size_t index)
        {
            if (!anchor_)
                anchor_ = index;
            flag_ = index;
        }

        /**
         * @brief Selects @p index on top of the range.
         */
        void add(std::size_t index)
        {
            removed_.erase(index);
            added_.insert(index);
        }

        /**
         * @brief Deselects @p index without touching the range.
         */
        void remove(std::size_t index)
        {
            added_.erase(index);
            if (inRange(index))
                removed_.insert(index);
        }

        /**
         * @brief Flips @p index through the mask, as a Ctrl+click does. The range never changes.
         */
        void toggleMasked(std::size_t index)
        {
            if (removed_.erase(index) > 0)
                return;
            if (added_.erase(index) > 0)
                return;
            if (inRange(index))
                removed_.insert(index);
            else
                added_.insert(index);
        }

        /**
         * @brief Keeps exactly @p selected as the selection, without a range. The anchor stays when it is one of
         *        them, so a following shift selection starts there. Called when the selectable indices change, so
         *        the range never takes in an index that could not be selected before.
         */
        void pin(std::set<std::size_t> selected)
        {
            flag_.reset();
            if (anchor_ && !selected.contains(*anchor_))
                anchor_.reset();
            removed_.clear();
            added_ = std::move(selected);
        }

        bool inRange(std::size_t index) const
        {
            if (!anchor_)
                return false;
            const auto flag = flag_.value_or(*anchor_);
            return index >= std::min(*anchor_, flag) && index <= std::max(*anchor_, flag);
        }

        /**
         * @brief Whether @p index belongs to the selection, ignoring whether it may be selected.
         */
        bool contains(std::size_t index) const
        {
            return (inRange(index) && !removed_.contains(index)) || added_.contains(index);
        }

        /**
         * @brief The selected indices below @p count for which @p isSelectable holds.
         */
        template <typename PredicateT>
        std::set<std::size_t> selectedIndices(std::size_t count, PredicateT&& isSelectable) const
        {
            std::set<std::size_t> result;
            for (std::size_t index = 0; index < count; ++index)
                if (contains(index) && isSelectable(index))
                    result.insert(index);
            return result;
        }

        std::optional<std::size_t> anchor() const
        {
            return anchor_;
        }

        std::optional<std::size_t> flag() const
        {
            return flag_;
        }

        /**
         * @brief The end the user moves: the flag, else the anchor, nullopt when there is neither.
         */
        std::optional<std::size_t> activeIndex() const
        {
            return flag_ ? flag_ : anchor_;
        }

      private:
        std::optional<std::size_t> anchor_{};
        std::optional<std::size_t> flag_{};
        std::set<std::size_t> added_{};
        std::set<std::size_t> removed_{};
    };
}
