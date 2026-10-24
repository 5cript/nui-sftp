#pragma once

#include <ids/ids.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace FileTracking
{
    /**
     * @brief The conflicts that wait for the user's answer, any number per watch but one per watch and local path.
     *        Each keeps a @p Context next to the shared conflict: what the tracker answers with, the upload that
     *        waits for example. Plain bookkeeping, it tells nobody.
     */
    template <typename Context>
    class ConflictList
    {
      public:
        /**
         * @brief A listed conflict with its context.
         */
        struct Listed
        {
            SharedData::WatchConflict conflict;
            Context context;
        };

        /**
         * @brief What add() listed.
         */
        struct Added
        {
            /** @brief The conflict as listed, with the id it got. */
            SharedData::WatchConflict conflict;
            /** @brief The context of the conflict it replaced, nothing when it is new. */
            std::optional<Context> replaced{};
        };

        /**
         * @brief Lists @p conflict of its watch and local path with @p context. A conflict listed there already is
         *        replaced: the new one keeps its id and arrives anew, behind the others of the watch. A new one gets a
         *        new id, the id of @p conflict is not used.
         */
        Added add(SharedData::WatchConflict conflict, Context context)
        {
            auto& index = watches_[conflict.watchId];
            std::optional<Context> replaced{};
            if (const auto listed = index.byPath.find(conflict.localPath); listed != index.byPath.end())
            {
                auto node = conflicts_.extract(listed->second);
                index.byArrival.erase(node.mapped().arrival);
                conflict.conflictId = node.key();
                replaced = std::move(node.mapped().listed.context);
            }
            else
                conflict.conflictId = Ids::generateConflictId();
            const auto arrival = nextArrival_++;
            index.byPath.insert_or_assign(conflict.localPath, conflict.conflictId);
            index.byArrival.emplace(arrival, conflict.conflictId);
            conflicts_.emplace(
                conflict.conflictId,
                Stored{.listed = Listed{.conflict = conflict, .context = std::move(context)}, .arrival = arrival}
            );
            return Added{.conflict = std::move(conflict), .replaced = std::move(replaced)};
        }

        /**
         * @brief The conflict @p conflictId, nothing when it is not listed.
         */
        Listed const* find(Ids::ConflictId const& conflictId) const
        {
            const auto found = conflicts_.find(conflictId);
            return found == conflicts_.end() ? nullptr : &found->second.listed;
        }

        /**
         * @brief The conflict of the watch @p watchId at @p localPath, nothing when none is listed there.
         */
        Listed const* find(Ids::WatchId const& watchId, Paths::LocalPath const& localPath) const
        {
            const auto* index = indexOf(watchId);
            if (!index)
                return nullptr;
            const auto found = index->byPath.find(localPath);
            return found == index->byPath.end() ? nullptr : find(found->second);
        }

        /**
         * @brief The conflicts of the watch @p watchId in the order they arrived.
         */
        std::vector<SharedData::WatchConflict> conflictsOf(Ids::WatchId const& watchId) const
        {
            std::vector<SharedData::WatchConflict> listed;
            if (const auto* index = indexOf(watchId))
                for (auto const& conflictId : index->byArrival | std::views::values)
                    listed.push_back(conflicts_.at(conflictId).listed.conflict);
            return listed;
        }

        /**
         * @brief How many conflicts of the watch @p watchId wait for an answer.
         */
        std::uint64_t countOf(Ids::WatchId const& watchId) const
        {
            const auto* index = indexOf(watchId);
            return index ? static_cast<std::uint64_t>(index->byArrival.size()) : 0u;
        }

        std::uint64_t size() const
        {
            return static_cast<std::uint64_t>(conflicts_.size());
        }

        bool empty() const
        {
            return conflicts_.empty();
        }

        /**
         * @brief Takes the conflict @p conflictId off the list.
         * @return It, nothing when it was not listed.
         */
        std::optional<Listed> remove(Ids::ConflictId const& conflictId)
        {
            auto node = conflicts_.extract(conflictId);
            if (node.empty())
                return std::nullopt;
            unindex(node.mapped());
            return std::move(node.mapped().listed);
        }

        /**
         * @brief Takes every conflict of the watch @p watchId off the list.
         * @return They, in the order they arrived.
         */
        std::vector<Listed> removeWatch(Ids::WatchId const& watchId)
        {
            auto node = watches_.extract(watchId);
            std::vector<Listed> removed;
            if (node.empty())
                return removed;
            for (auto const& conflictId : node.mapped().byArrival | std::views::values)
                removed.push_back(std::move(conflicts_.extract(conflictId).mapped().listed));
            return removed;
        }

        /**
         * @brief Takes every conflict off the list.
         * @return They, in the order they arrived.
         */
        std::vector<Listed> removeAll()
        {
            auto conflicts = std::exchange(conflicts_, {});
            watches_.clear();
            std::vector<Stored> stored;
            stored.reserve(conflicts.size());
            for (auto& listed : conflicts | std::views::values)
                stored.push_back(std::move(listed));
            std::ranges::sort(stored, std::ranges::less{}, &Stored::arrival);
            std::vector<Listed> removed;
            removed.reserve(stored.size());
            for (auto& listed : stored)
                removed.push_back(std::move(listed.listed));
            return removed;
        }

      private:
        struct Stored
        {
            Listed listed;
            std::uint64_t arrival{0u};
        };

        /**
         * @brief The conflicts of one watch by local path and in the order they arrived.
         */
        struct WatchIndex
        {
            std::map<Paths::LocalPath, Ids::ConflictId> byPath{};
            std::map<std::uint64_t, Ids::ConflictId> byArrival{};
        };

        WatchIndex const* indexOf(Ids::WatchId const& watchId) const
        {
            const auto found = watches_.find(watchId);
            return found == watches_.end() ? nullptr : &found->second;
        }

        void unindex(Stored const& stored)
        {
            const auto index = watches_.find(stored.listed.conflict.watchId);
            if (index == watches_.end())
                return;
            index->second.byPath.erase(stored.listed.conflict.localPath);
            index->second.byArrival.erase(stored.arrival);
            if (index->second.byArrival.empty())
                watches_.erase(index);
        }

        std::map<Ids::ConflictId, Stored> conflicts_{};
        std::map<Ids::WatchId, WatchIndex> watches_{};
        std::uint64_t nextArrival_{0u};
    };
}
