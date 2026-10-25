#include <frontend/session_components/file_tracking/tracking_model.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

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

        /**
         * @return Whether a watch id is one of @p entries.
         */
        auto knownIn(std::map<Ids::WatchId, SharedData::TrackedEntry> const& entries)
        {
            return [&entries](Ids::WatchId const& watchId)
            {
                return entries.contains(watchId);
            };
        }

        template <typename ProblemsT>
        std::uint64_t countOf(ProblemsT const& problems, Ids::WatchId const& watchId)
        {
            const auto list = problems.lists.find(watchId);
            return list == problems.lists.end() ? 0u : static_cast<std::uint64_t>(list->second.size());
        }

        template <typename ProblemsT, typename IdT>
        std::optional<Ids::WatchId> watchOf(ProblemsT const& problems, IdT const& problemId)
        {
            const auto found = problems.watches.find(problemId);
            if (found == problems.watches.end())
                return std::nullopt;
            return found->second;
        }

        template <typename ProblemsT>
        auto sliceOf(ProblemsT const& problems, Ids::WatchId const& watchId, std::size_t offset, std::size_t count)
            -> decltype(problems.lists.begin()->second.slice(offset, count))
        {
            const auto list = problems.lists.find(watchId);
            if (list == problems.lists.end())
                return {};
            return list->second.slice(offset, count);
        }

        /**
         * @brief Removes @p problemIds (one id or a vector of them) from the list of @p watchId, dropping the list
         *        once it is empty. The caller keeps the id to watch lookup.
         */
        template <typename ProblemsT, typename IdsT>
        void removeFromWatch(ProblemsT& problems, Ids::WatchId const& watchId, IdsT const& problemIds, bool known)
        {
            const auto list = problems.lists.find(watchId);
            if (list == problems.lists.end())
                return;
            const auto removed = static_cast<std::uint64_t>(list->second.remove(problemIds));
            if (known)
                problems.knownTotal -= removed;
            if (list->second.empty())
                problems.lists.erase(list);
        }

        template <typename ProblemsT, typename IdT, typename ValueT, typename IsKnownT>
        void showProblem(
            ProblemsT& problems,
            IdT const& problemId,
            ValueT const& value,
            std::uint64_t sequence,
            IsKnownT const& isKnown
        )
        {
            const auto [watch, inserted] = problems.watches.try_emplace(problemId, value.watchId);
            if (!inserted && watch->second != value.watchId)
            {
                removeFromWatch(problems, watch->second, problemId, isKnown(watch->second));
                watch->second = value.watchId;
            }
            if (problems.lists[value.watchId].show(problemId, value, sequence) && isKnown(value.watchId))
                ++problems.knownTotal;
        }

        /**
         * @brief Removes @p problemIds, one pass over the list of each watch they belong to.
         */
        template <typename ProblemsT, typename IdT, typename IsKnownT>
        void removeProblems(ProblemsT& problems, std::vector<IdT> const& problemIds, IsKnownT const& isKnown)
        {
            std::map<Ids::WatchId, std::vector<IdT>> byWatch{};
            for (auto const& problemId : problemIds)
            {
                const auto watch = problems.watches.find(problemId);
                if (watch == problems.watches.end())
                    continue;
                byWatch[watch->second].push_back(problemId);
                problems.watches.erase(watch);
            }
            for (auto const& [watchId, ofWatch] : byWatch)
                removeFromWatch(problems, watchId, ofWatch, isKnown(watchId));
        }

        template <typename ProblemsT>
        void dropWatch(ProblemsT& problems, Ids::WatchId const& watchId, bool known)
        {
            const auto list = problems.lists.find(watchId);
            if (list == problems.lists.end())
                return;
            for (auto const& problem : list->second)
                problems.watches.erase(problem.id);
            if (known)
                problems.knownTotal -= static_cast<std::uint64_t>(list->second.size());
            problems.lists.erase(list);
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
        const auto [shown, inserted] = entries_.insert_or_assign(entry.watchId, entry);
        if (!inserted)
            return;
        conflicts_.knownTotal += countOf(conflicts_, entry.watchId);
        failures_.knownTotal += countOf(failures_, entry.watchId);
    }

    void TrackingModel::removeEntry(Ids::WatchId const& watchId)
    {
        const auto known = entries_.erase(watchId) > 0u;
        progress_.erase(watchId);
        dropWatch(conflicts_, watchId, known);
        dropWatch(failures_, watchId, known);
    }

    void TrackingModel::showConflict(SharedData::WatchConflict const& conflict)
    {
        showProblem(conflicts_, conflict.conflictId, conflict, ++sequence_, knownIn(entries_));
    }

    void TrackingModel::removeConflicts(std::vector<Ids::ConflictId> const& conflictIds)
    {
        removeProblems(conflicts_, conflictIds, knownIn(entries_));
    }

    void TrackingModel::showFailure(SharedData::UploadFailure const& failure)
    {
        showProblem(failures_, failure.failureId, failure, ++sequence_, knownIn(entries_));
    }

    void TrackingModel::removeFailures(std::vector<Ids::FailureId> const& failureIds)
    {
        removeProblems(failures_, failureIds, knownIn(entries_));
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
        conflicts_ = {};
        failures_ = {};
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

    std::vector<SharedData::WatchConflict>
    TrackingModel::conflictsOf(Ids::WatchId const& watchId, std::size_t offset, std::size_t count) const
    {
        return sliceOf(conflicts_, watchId, offset, count);
    }

    std::vector<SharedData::UploadFailure>
    TrackingModel::failuresOf(Ids::WatchId const& watchId, std::size_t offset, std::size_t count) const
    {
        return sliceOf(failures_, watchId, offset, count);
    }

    std::uint64_t TrackingModel::conflictCountOf(Ids::WatchId const& watchId) const
    {
        return countOf(conflicts_, watchId);
    }

    std::uint64_t TrackingModel::failureCountOf(Ids::WatchId const& watchId) const
    {
        return countOf(failures_, watchId);
    }

    std::uint64_t TrackingModel::conflictCount() const
    {
        return conflicts_.knownTotal;
    }

    std::uint64_t TrackingModel::failureCount() const
    {
        return failures_.knownTotal;
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
        ConflictList::ListedProblem const* newest = nullptr;
        for (auto const& [watchId, list] : conflicts_.lists)
        {
            auto const* candidate = list.newest();
            if (candidate != nullptr && entries_.contains(watchId) &&
                (newest == nullptr || candidate->sequence > newest->sequence))
                newest = candidate;
        }
        if (newest == nullptr)
            return std::nullopt;
        return newest->value;
    }
}
