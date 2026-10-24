#pragma once

#include <ids/ids.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

namespace FileTracking
{
    /**
     * @brief The failed uploads of the watches, one per watch and local path, in the order their paths first failed.
     *        They wait for Retry or Dismiss, nothing retries them. Plain bookkeeping, it tells nobody.
     */
    class FailureList
    {
      public:
        /**
         * @brief Lists @p failure of its watch and local path. A path that failed before keeps its id and place,
         *        takes the reason, detail, remote path and failedAt of @p failure, and counts one attempt more. A new
         *        one gets a new id and one attempt, those of @p failure are not used.
         * @return The failure as listed.
         */
        SharedData::UploadFailure record(SharedData::UploadFailure failure)
        {
            auto& index = watches_[failure.watchId];
            if (const auto listed = index.byPath.find(failure.localPath); listed != index.byPath.end())
            {
                auto& stored = failures_.at(listed->second).failure;
                failure.failureId = stored.failureId;
                failure.attempts = stored.attempts + 1u;
                stored = failure;
                return failure;
            }
            failure.failureId = Ids::generateFailureId();
            failure.attempts = 1u;
            const auto arrival = nextArrival_++;
            index.byPath.emplace(failure.localPath, failure.failureId);
            index.byArrival.emplace(arrival, failure.failureId);
            failures_.emplace(failure.failureId, Stored{.failure = failure, .arrival = arrival});
            return failure;
        }

        /**
         * @brief The failure @p failureId, nothing when it is not listed.
         */
        SharedData::UploadFailure const* find(Ids::FailureId const& failureId) const
        {
            const auto found = failures_.find(failureId);
            return found == failures_.end() ? nullptr : &found->second.failure;
        }

        /**
         * @brief The failure of the watch @p watchId at @p localPath, nothing when none is listed there.
         */
        SharedData::UploadFailure const* find(Ids::WatchId const& watchId, Paths::LocalPath const& localPath) const
        {
            const auto* index = indexOf(watchId);
            if (!index)
                return nullptr;
            const auto found = index->byPath.find(localPath);
            return found == index->byPath.end() ? nullptr : find(found->second);
        }

        /**
         * @brief The failures of the watch @p watchId in the order their paths first failed.
         */
        std::vector<SharedData::UploadFailure> failuresOf(Ids::WatchId const& watchId) const
        {
            std::vector<SharedData::UploadFailure> listed;
            if (const auto* index = indexOf(watchId))
                for (auto const& failureId : index->byArrival | std::views::values)
                    listed.push_back(failures_.at(failureId).failure);
            return listed;
        }

        std::uint64_t countOf(Ids::WatchId const& watchId) const
        {
            const auto* index = indexOf(watchId);
            return index ? static_cast<std::uint64_t>(index->byArrival.size()) : 0u;
        }

        std::uint64_t size() const
        {
            return static_cast<std::uint64_t>(failures_.size());
        }

        bool empty() const
        {
            return failures_.empty();
        }

        /**
         * @brief Takes the failure @p failureId off the list, on Dismiss.
         * @return It, nothing when it was not listed.
         */
        std::optional<SharedData::UploadFailure> remove(Ids::FailureId const& failureId)
        {
            auto node = failures_.extract(failureId);
            if (node.empty())
                return std::nullopt;
            unindex(node.mapped());
            return std::move(node.mapped().failure);
        }

        /**
         * @brief Takes the failure of the watch @p watchId at @p localPath off the list, once that path uploaded.
         * @return It, nothing when none was listed there.
         */
        std::optional<SharedData::UploadFailure>
        removePath(Ids::WatchId const& watchId, Paths::LocalPath const& localPath)
        {
            const auto* failure = find(watchId, localPath);
            if (!failure)
                return std::nullopt;
            const auto failureId = failure->failureId;
            return remove(failureId);
        }

        /**
         * @brief Takes the failures of the watch @p watchId at @p directory and below it off the list, once a
         *        directory upload went through, but for the paths in @p keep: those it left out.
         * @return They, in the order their paths first failed.
         */
        std::vector<SharedData::UploadFailure> removeWithin(
            Ids::WatchId const& watchId,
            Paths::LocalPath const& directory,
            std::set<Paths::LocalPath> const& keep = {}
        )
        {
            std::vector<Ids::FailureId> within;
            if (const auto* index = indexOf(watchId))
                for (auto const& failureId : index->byArrival | std::views::values)
                {
                    auto const& path = failures_.at(failureId).failure.localPath;
                    if (path.isWithin(directory) && !keep.contains(path))
                        within.push_back(failureId);
                }
            std::vector<SharedData::UploadFailure> removed;
            for (auto const& failureId : within)
                removed.push_back(*remove(failureId));
            return removed;
        }

        /**
         * @brief Takes every failure of the watch @p watchId off the list.
         * @return They, in the order their paths first failed.
         */
        std::vector<SharedData::UploadFailure> removeWatch(Ids::WatchId const& watchId)
        {
            auto node = watches_.extract(watchId);
            std::vector<SharedData::UploadFailure> removed;
            if (node.empty())
                return removed;
            for (auto const& failureId : node.mapped().byArrival | std::views::values)
                removed.push_back(std::move(failures_.extract(failureId).mapped().failure));
            return removed;
        }

        /**
         * @brief Takes every failure off the list.
         * @return They, in the order their paths first failed.
         */
        std::vector<SharedData::UploadFailure> removeAll()
        {
            auto failures = std::exchange(failures_, {});
            watches_.clear();
            std::map<std::uint64_t, SharedData::UploadFailure> byArrival;
            for (auto& stored : failures | std::views::values)
                byArrival.emplace(stored.arrival, std::move(stored.failure));
            std::vector<SharedData::UploadFailure> removed;
            removed.reserve(byArrival.size());
            for (auto& failure : byArrival | std::views::values)
                removed.push_back(std::move(failure));
            return removed;
        }

      private:
        struct Stored
        {
            SharedData::UploadFailure failure;
            std::uint64_t arrival{0u};
        };

        /**
         * @brief The failures of one watch by local path and in the order their paths first failed.
         */
        struct WatchIndex
        {
            std::map<Paths::LocalPath, Ids::FailureId> byPath{};
            std::map<std::uint64_t, Ids::FailureId> byArrival{};
        };

        WatchIndex const* indexOf(Ids::WatchId const& watchId) const
        {
            const auto found = watches_.find(watchId);
            return found == watches_.end() ? nullptr : &found->second;
        }

        void unindex(Stored const& stored)
        {
            const auto index = watches_.find(stored.failure.watchId);
            if (index == watches_.end())
                return;
            index->second.byPath.erase(stored.failure.localPath);
            index->second.byArrival.erase(stored.arrival);
            if (index->second.byArrival.empty())
                watches_.erase(index);
        }

        std::map<Ids::FailureId, Stored> failures_{};
        std::map<Ids::WatchId, WatchIndex> watches_{};
        std::uint64_t nextArrival_{0u};
    };
}
