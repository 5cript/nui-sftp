#include <frontend/session_components/file_tracking/tracking_model.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace FileTracking
{
    namespace
    {
        char foldedCase(char character)
        {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        }

        std::string foldedName(std::string_view name)
        {
            std::string folded(name.size(), '\0');
            std::ranges::transform(name, folded.begin(), foldedCase);
            return folded;
        }

        /**
         * @brief A member of an outliner group with what it is sorted by.
         */
        template <typename IdT>
        struct NamedMember
        {
            std::string foldedName;
            IdT id;
        };

        template <typename IdT>
        std::vector<Selection> inNameOrder(std::vector<NamedMember<IdT>> members)
        {
            std::ranges::sort(
                members,
                [](NamedMember<IdT> const& left, NamedMember<IdT> const& right)
                {
                    if (left.foldedName != right.foldedName)
                        return left.foldedName < right.foldedName;
                    return left.id < right.id;
                }
            );
            std::vector<Selection> result;
            result.reserve(members.size());
            std::ranges::transform(
                members,
                std::back_inserter(result),
                [](NamedMember<IdT> const& member)
                {
                    return Selection{std::in_place_type<IdT>, member.id};
                }
            );
            return result;
        }

        bool isNewer(SharedData::WatchConflict const& conflict, SharedData::WatchConflict const& other)
        {
            if (conflict.raisedAt != other.raisedAt)
                return conflict.raisedAt > other.raisedAt;
            return conflict.conflictId > other.conflictId;
        }

        bool isNewer(SharedData::UploadFailure const& failure, SharedData::UploadFailure const& other)
        {
            if (failure.failedAt != other.failedAt)
                return failure.failedAt > other.failedAt;
            return failure.failureId > other.failureId;
        }

        template <typename MapT>
        std::vector<typename MapT::mapped_type> newestFirstOf(MapT const& items, Ids::WatchId const& watchId)
        {
            std::vector<typename MapT::mapped_type> result;
            for (auto const& item : items | std::views::values)
            {
                if (item.watchId == watchId)
                    result.push_back(item);
            }
            std::ranges::sort(
                result,
                [](auto const& left, auto const& right)
                {
                    return isNewer(left, right);
                }
            );
            return result;
        }

        template <typename MapT, typename PredicateT>
        std::uint64_t countOf(MapT const& items, PredicateT predicate)
        {
            return static_cast<std::uint64_t>(std::ranges::count_if(items | std::views::values, predicate));
        }

        template <typename MapT, typename IdT>
        std::optional<Ids::WatchId> watchOf(MapT const& items, IdT const& itemId)
        {
            const auto found = items.find(itemId);
            if (found == items.end())
                return std::nullopt;
            return found->second.watchId;
        }

        bool isHeldByAnEntry(
            std::map<Ids::WatchId, SharedData::TrackedEntry> const& entries,
            Ids::InstanceId const& instanceId
        )
        {
            return std::ranges::any_of(
                entries | std::views::values,
                [&instanceId](SharedData::TrackedEntry const& entry)
                {
                    return entry.instanceId == instanceId;
                }
            );
        }
    }

    bool matchesFilter(std::string_view name, std::string_view filter)
    {
        const auto equalIgnoringCase = [](char left, char right)
        {
            return foldedCase(left) == foldedCase(right);
        };
        return filter.empty() || !std::ranges::search(name, filter, equalIgnoringCase).empty();
    }

    void TrackingModel::showEntry(SharedData::TrackedEntry const& entry)
    {
        entries_.insert_or_assign(entry.watchId, entry);
    }

    void TrackingModel::removeEntry(Ids::WatchId const& watchId)
    {
        const auto removed = watchId;
        const auto ofRemoved = [&removed](auto const& item)
        {
            return item.second.watchId == removed;
        };
        entries_.erase(removed);
        progress_.erase(removed);
        std::erase_if(conflicts_, ofRemoved);
        std::erase_if(failures_, ofRemoved);
    }

    void TrackingModel::showConflict(SharedData::WatchConflict const& conflict)
    {
        conflicts_.insert_or_assign(conflict.conflictId, conflict);
    }

    void TrackingModel::removeConflict(Ids::ConflictId const& conflictId)
    {
        conflicts_.erase(conflictId);
    }

    void TrackingModel::showFailure(SharedData::UploadFailure const& failure)
    {
        failures_.insert_or_assign(failure.failureId, failure);
    }

    void TrackingModel::removeFailure(Ids::FailureId const& failureId)
    {
        failures_.erase(failureId);
    }

    void TrackingModel::showWatchProgress(SharedData::WatchProgress const& progress)
    {
        progress_.insert_or_assign(progress.watchId, progress);
    }

    void TrackingModel::showOrphanedCopies(SharedData::OrphanedCopies const& copies)
    {
        orphans_ = copies.copies;
    }

    void TrackingModel::clear()
    {
        entries_.clear();
        progress_.clear();
        conflicts_.clear();
        failures_.clear();
        orphans_.clear();
    }

    SharedData::TrackedEntry const* TrackingModel::entryOf(Ids::WatchId const& watchId) const
    {
        const auto found = entries_.find(watchId);
        return found == entries_.end() ? nullptr : &found->second;
    }

    SharedData::WatchProgress const* TrackingModel::progressOf(Ids::WatchId const& watchId) const
    {
        const auto found = progress_.find(watchId);
        return found == progress_.end() ? nullptr : &found->second;
    }

    SharedData::OrphanedCopy const* TrackingModel::orphanOf(Ids::InstanceId const& instanceId) const
    {
        if (isHeldByAnEntry(entries_, instanceId))
            return nullptr;
        const auto found = std::ranges::find_if(
            orphans_,
            [&instanceId](SharedData::OrphanedCopy const& copy)
            {
                return copy.instanceId == instanceId;
            }
        );
        return found == orphans_.end() ? nullptr : &*found;
    }

    std::optional<Ids::WatchId> TrackingModel::watchOfConflict(Ids::ConflictId const& conflictId) const
    {
        return watchOf(conflicts_, conflictId);
    }

    std::optional<Ids::WatchId> TrackingModel::watchOfFailure(Ids::FailureId const& failureId) const
    {
        return watchOf(failures_, failureId);
    }

    std::vector<SharedData::WatchConflict> TrackingModel::conflictsOf(Ids::WatchId const& watchId) const
    {
        return newestFirstOf(conflicts_, watchId);
    }

    std::vector<SharedData::UploadFailure> TrackingModel::failuresOf(Ids::WatchId const& watchId) const
    {
        return newestFirstOf(failures_, watchId);
    }

    std::uint64_t TrackingModel::conflictCountOf(Ids::WatchId const& watchId) const
    {
        return countOf(
            conflicts_,
            [&watchId](SharedData::WatchConflict const& conflict)
            {
                return conflict.watchId == watchId;
            }
        );
    }

    std::uint64_t TrackingModel::failureCountOf(Ids::WatchId const& watchId) const
    {
        return countOf(
            failures_,
            [&watchId](SharedData::UploadFailure const& failure)
            {
                return failure.watchId == watchId;
            }
        );
    }

    std::uint64_t TrackingModel::conflictCount() const
    {
        return countOf(
            conflicts_,
            [this](SharedData::WatchConflict const& conflict)
            {
                return entries_.contains(conflict.watchId);
            }
        );
    }

    std::uint64_t TrackingModel::failureCount() const
    {
        return countOf(
            failures_,
            [this](SharedData::UploadFailure const& failure)
            {
                return entries_.contains(failure.watchId);
            }
        );
    }

    WatchStatus TrackingModel::statusOf(Ids::WatchId const& watchId) const
    {
        auto const* entry = entryOf(watchId);
        if (entry == nullptr)
            return WatchStatus::Watching;
        if (entry->stop.has_value())
            return WatchStatus::Stopped;
        if (conflictCountOf(watchId) > 0u)
            return WatchStatus::Conflicts;
        if (failureCountOf(watchId) > 0u)
            return WatchStatus::Failed;
        auto const* progress = progressOf(watchId);
        if (entry->uploading || (progress != nullptr && progress->changesPending > 0u))
            return WatchStatus::Syncing;
        return WatchStatus::Watching;
    }

    OutlinerGroup TrackingModel::groupOf(Ids::WatchId const& watchId) const
    {
        switch (statusOf(watchId))
        {
            case WatchStatus::Stopped:
                return OutlinerGroup::Stopped;
            case WatchStatus::Conflicts:
            case WatchStatus::Failed:
                return OutlinerGroup::NeedsYou;
            case WatchStatus::Syncing:
                return OutlinerGroup::Syncing;
            case WatchStatus::Watching:
                return OutlinerGroup::Watching;
        }
        return OutlinerGroup::Watching;
    }

    std::vector<Selection> TrackingModel::members(OutlinerGroup group, std::string_view filter) const
    {
        if (group == OutlinerGroup::Orphaned)
        {
            std::vector<NamedMember<Ids::InstanceId>> copies;
            for (auto const& copy : orphans_)
            {
                if (!isHeldByAnEntry(entries_, copy.instanceId) && matchesFilter(copy.name, filter))
                    copies.push_back({.foldedName = foldedName(copy.name), .id = copy.instanceId});
            }
            return inNameOrder(std::move(copies));
        }
        std::vector<NamedMember<Ids::WatchId>> watches;
        for (auto const& [watchId, entry] : entries_)
        {
            if (groupOf(watchId) == group && matchesFilter(entry.name, filter))
                watches.push_back({.foldedName = foldedName(entry.name), .id = watchId});
        }
        return inNameOrder(std::move(watches));
    }

    std::string TrackingModel::sideTextOf(Ids::WatchId const& watchId, FileTrackingTexts const& texts) const
    {
        const auto conflicts = conflictCountOf(watchId);
        if (conflicts > 0u)
            return conflictCountText(texts, conflicts);
        const auto failures = failureCountOf(watchId);
        if (failures > 0u)
            return failedCountText(texts, failures);
        auto const* entry = entryOf(watchId);
        if (entry != nullptr && entry->stop.has_value())
            return stopSideText(texts, entry->stop->reason);
        return {};
    }

    std::string TrackingModel::statusTextOf(Ids::WatchId const& watchId, FileTrackingTexts const& texts) const
    {
        switch (statusOf(watchId))
        {
            case WatchStatus::Conflicts:
                return conflictCountText(texts, conflictCountOf(watchId));
            case WatchStatus::Failed:
                return failedCountText(texts, failureCountOf(watchId));
            case WatchStatus::Syncing:
                return texts.statuses.syncing;
            case WatchStatus::Stopped:
                return texts.statuses.stopped;
            case WatchStatus::Watching:
                return texts.statuses.upToDate;
        }
        return texts.statuses.upToDate;
    }

    double TrackingModel::ringFractionOf(Ids::WatchId const& watchId) const
    {
        auto const* progress = progressOf(watchId);
        if (progress == nullptr)
            return 0.0;
        if (progress->bytesTotal > 0u)
            return std::clamp(
                static_cast<double>(progress->bytesDone) / static_cast<double>(progress->bytesTotal), 0.0, 1.0
            );
        const auto changes = progress->changesDone + progress->changesPending;
        if (changes == 0u)
            return 0.0;
        return static_cast<double>(progress->changesDone) / static_cast<double>(changes);
    }

    std::optional<SharedData::WatchConflict> TrackingModel::newestConflict() const
    {
        std::optional<SharedData::WatchConflict> newest;
        for (auto const& conflict : conflicts_ | std::views::values)
        {
            if (entries_.contains(conflict.watchId) && (!newest.has_value() || isNewer(conflict, *newest)))
                newest = conflict;
        }
        return newest;
    }
}
