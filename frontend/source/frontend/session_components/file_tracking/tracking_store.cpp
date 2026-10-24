#include <frontend/session_components/file_tracking/tracking_store.hpp>
#include <frontend/session_components/file_tracking/in_flight_rows.hpp>
#include <utility/describe.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/traits_and_concepts/optional.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace FileTracking
{
    namespace
    {
        template <typename T>
        struct IsVector : std::false_type
        {};

        template <typename T, typename AllocatorT>
        struct IsVector<std::vector<T, AllocatorT>> : std::true_type
        {};

        /**
         * @brief Compares described structs member by member, optionals and vectors element by element, everything
         *        else with ==. The shared types have no operator== of their own.
         */
        template <typename T>
        bool sameValue(T const& lhs, T const& rhs)
        {
            if constexpr (boost::describe::has_describe_members<T>::value)
            {
                auto same = true;
                boost::mp11::mp_for_each<boost::describe::describe_members<T, boost::describe::mod_public>>(
                    [&](auto member)
                    {
                        same = same && sameValue(lhs.*member.pointer, rhs.*member.pointer);
                    }
                );
                return same;
            }
            else if constexpr (Utility::IsOptional_v<T>)
                return lhs.has_value() == rhs.has_value() && (!lhs.has_value() || sameValue(*lhs, *rhs));
            else if constexpr (IsVector<T>::value)
                return std::ranges::equal(
                    lhs,
                    rhs,
                    [](auto const& left, auto const& right)
                    {
                        return sameValue(left, right);
                    }
                );
            else
                return lhs == rhs;
        }

        /**
         * @brief Assigns @p value to @p observed only when it differs, so nothing that shows it updates in vain.
         */
        template <typename ValueT, typename OtherT>
        requires std::same_as<std::remove_cvref_t<OtherT>, ValueT>
        void assignIfChanged(Nui::Observed<ValueT>& observed, OtherT&& value)
        {
            if (sameValue(observed.value(), value))
                return;
            observed = std::forward<OtherT>(value);
        }

        /**
         * @return For each of @p values whether it belongs to one longest increasing subsequence of them. The values
         *         are distinct.
         */
        std::vector<bool> longestIncreasing(std::vector<std::size_t> const& values)
        {
            std::vector<std::size_t> tails{};
            std::vector<std::optional<std::size_t>> previous(values.size());
            for (std::size_t index = 0u; index < values.size(); ++index)
            {
                const auto tail = std::ranges::lower_bound(
                    tails,
                    values[index],
                    {},
                    [&values](std::size_t tailIndex)
                    {
                        return values[tailIndex];
                    }
                );
                const auto length = static_cast<std::size_t>(std::distance(tails.begin(), tail));
                if (length > 0u)
                    previous[index] = tails[length - 1u];
                if (length == tails.size())
                    tails.push_back(index);
                else
                    tails[length] = index;
            }
            std::vector<bool> kept(values.size(), false);
            auto current = tails.empty() ? std::nullopt : std::optional<std::size_t>{tails.back()};
            while (current)
            {
                kept[*current] = true;
                current = previous[*current];
            }
            return kept;
        }

        /**
         * @brief Turns @p observed into @p target with the fewest erases and inserts, comparing items by identity:
         *        the longest run of items already in target order stays, everything else is erased or inserted. A
         *        range over @p observed then renders only what moved, came or went.
         */
        template <typename ItemT>
        void syncList(
            Nui::Observed<std::vector<std::shared_ptr<ItemT>>>& observed,
            std::vector<std::shared_ptr<ItemT>> const& target
        )
        {
            auto const& current = observed.value();
            if (current == target)
                return;

            std::unordered_map<ItemT const*, std::size_t> targetPositions{};
            for (std::size_t index = 0u; index < target.size(); ++index)
                targetPositions.emplace(target[index].get(), index);

            std::vector<std::size_t> wantedIndices{};
            std::vector<std::size_t> wantedPositions{};
            for (std::size_t index = 0u; index < current.size(); ++index)
            {
                if (const auto found = targetPositions.find(current[index].get()); found != targetPositions.end())
                {
                    wantedIndices.push_back(index);
                    wantedPositions.push_back(found->second);
                }
            }
            const auto stable = longestIncreasing(wantedPositions);
            std::vector<bool> keep(current.size(), false);
            for (std::size_t wanted = 0u; wanted < wantedIndices.size(); ++wanted)
                keep[wantedIndices[wanted]] = stable[wanted];

            auto runEnd = current.size();
            while (runEnd > 0u)
            {
                if (keep[runEnd - 1u])
                {
                    --runEnd;
                    continue;
                }
                auto runBegin = runEnd - 1u;
                while (runBegin > 0u && !keep[runBegin - 1u])
                    --runBegin;
                observed.erase(
                    current.cbegin() + static_cast<std::ptrdiff_t>(runBegin),
                    current.cbegin() + static_cast<std::ptrdiff_t>(runEnd)
                );
                runEnd = runBegin;
            }

            std::size_t index = 0u;
            while (index < target.size())
            {
                if (index < current.size() && current[index] == target[index])
                {
                    ++index;
                    continue;
                }
                auto insertEnd = index + 1u;
                while (insertEnd < target.size() && (index >= current.size() || current[index] != target[insertEnd]))
                    ++insertEnd;
                observed.insert(
                    current.cbegin() + static_cast<std::ptrdiff_t>(index),
                    target.begin() + static_cast<std::ptrdiff_t>(index),
                    target.begin() + static_cast<std::ptrdiff_t>(insertEnd)
                );
                index = insertEnd;
            }
        }

        /**
         * @brief Updates the rows of @p observed from @p values in their order: an item whose value did not change is
         *        kept, a changed or new one is made by @p makeItem, all of them when @p rebuild is set.
         */
        template <typename ItemT, typename ValueT, typename KeyOfT, typename MakeItemT>
        void syncRows(
            Nui::Observed<std::vector<std::shared_ptr<ItemT>>>& observed,
            std::vector<ValueT> const& values,
            ValueT ItemT::* valueOfItem,
            KeyOfT keyOf,
            MakeItemT makeItem,
            bool rebuild
        )
        {
            using KeyT = std::remove_cvref_t<std::invoke_result_t<KeyOfT, ValueT const&>>;
            std::map<KeyT, std::shared_ptr<ItemT>> known{};
            if (!rebuild)
            {
                for (auto const& item : observed.value())
                    known.emplace(keyOf((*item).*valueOfItem), item);
            }
            std::vector<std::shared_ptr<ItemT>> target{};
            target.reserve(values.size());
            for (auto const& value : values)
            {
                const auto found = known.find(keyOf(value));
                if (found != known.end() && sameValue((*found->second).*valueOfItem, value))
                    target.push_back(found->second);
                else
                    target.push_back(std::make_shared<ItemT>(makeItem(value)));
            }
            syncList(observed, target);
        }

        std::size_t indexOf(OutlinerGroup group)
        {
            return static_cast<std::size_t>(group);
        }

        /**
         * @brief What of a watch an event changed, beside its status, texts and group, which always follow.
         */
        struct WatchParts
        {
            bool entry{false};
            bool progress{false};
            bool conflicts{false};
            bool failures{false};
        };

        constexpr WatchParts everyPart{.entry = true, .progress = true, .conflicts = true, .failures = true};
    }

    struct TrackingStore::Implementation
    {
        /**
         * @brief A known watch: its item, the group it is listed in, and the rows of its files in flight.
         */
        struct WatchRecord
        {
            std::shared_ptr<WatchItem> item{};
            std::optional<OutlinerGroup> group{};
            InFlightRows<std::string> rows{};
        };

        explicit Implementation(FileTrackingTexts texts)
            : texts{std::move(texts)}
        {}

        FileTrackingTexts texts;
        TrackingModel model{};
        std::map<Ids::WatchId, WatchRecord> watches{};
        std::map<Ids::InstanceId, std::shared_ptr<OrphanItem>> orphanItems{};
        std::array<Nui::Observed<std::vector<std::shared_ptr<WatchItem>>>, outlinerGroupOrder.size()> members{};
        Nui::Observed<std::vector<std::shared_ptr<OrphanItem>>> orphans{};
        std::array<std::shared_ptr<Nui::Observed<std::uint64_t>>, outlinerGroupOrder.size()> counts{
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
            std::make_shared<Nui::Observed<std::uint64_t>>(0u),
        };
        Nui::Observed<Selection> selection{};
        DetailTab detailTab{DetailTab::Conflicts};
        std::shared_ptr<Nui::Observed<std::string>> detailTabKey{
            std::make_shared<Nui::Observed<std::string>>(Utility::enumToString(DetailTab::Conflicts))
        };
        Nui::Observed<std::string> filter{};
        /** @brief The filter the member lists follow, the observed one may run ahead of it while being typed. */
        std::string appliedFilter{};
        std::array<bool, outlinerGroupOrder.size()> expanded{true, true, true, true, true};
        std::function<void(AttentionCounts const& counts)> onAttentionChanged{};
        AttentionCounts attentionCounts{};

        void selectNothing()
        {
            if (!std::holds_alternative<std::monostate>(selection.value()))
                selection = Selection{};
        }

        void notifyAttention()
        {
            const AttentionCounts current{.conflicts = model.conflictCount(), .failed = model.failureCount()};
            if (current == attentionCounts)
                return;
            attentionCounts = current;
            if (onAttentionChanged)
                onAttentionChanged(current);
        }

        void refreshWatchMembers(OutlinerGroup group)
        {
            std::vector<std::shared_ptr<WatchItem>> target{};
            for (auto const& member : model.members(group, appliedFilter))
            {
                if (auto const* watchId = std::get_if<Ids::WatchId>(&member))
                {
                    if (const auto watch = watches.find(*watchId); watch != watches.end())
                        target.push_back(watch->second.item);
                }
            }
            syncList(members[indexOf(group)], target);
            assignIfChanged(*counts[indexOf(group)], static_cast<std::uint64_t>(target.size()));
        }

        void refreshOrphanMembers()
        {
            std::vector<std::shared_ptr<OrphanItem>> target{};
            for (auto const& member : model.members(OutlinerGroup::Orphaned, appliedFilter))
            {
                if (auto const* instanceId = std::get_if<Ids::InstanceId>(&member))
                {
                    if (const auto orphan = orphanItems.find(*instanceId); orphan != orphanItems.end())
                        target.push_back(orphan->second);
                }
            }
            syncList(orphans, target);
            assignIfChanged(*counts[indexOf(OutlinerGroup::Orphaned)], static_cast<std::uint64_t>(target.size()));
        }

        void refreshMembers(OutlinerGroup group)
        {
            if (group == OutlinerGroup::Orphaned)
                refreshOrphanMembers();
            else
                refreshWatchMembers(group);
        }

        void refreshEveryMembers()
        {
            for (const auto group : outlinerGroupOrder)
                refreshMembers(group);
        }

        /**
         * @brief Rebuilds the orphan items from the model, keeping the items of copies still listed, and drops the
         *        selection of a copy that is gone.
         */
        void refreshOrphans()
        {
            std::map<Ids::InstanceId, std::shared_ptr<OrphanItem>> listed{};
            for (auto const& member : model.members(OutlinerGroup::Orphaned, {}))
            {
                auto const* instanceId = std::get_if<Ids::InstanceId>(&member);
                if (instanceId == nullptr)
                    continue;
                auto const* copy = model.orphanOf(*instanceId);
                if (copy == nullptr)
                    continue;
                const auto known = orphanItems.find(*instanceId);
                auto item = known != orphanItems.end() ? known->second : std::make_shared<OrphanItem>(*instanceId);
                assignIfChanged(item->copy, *copy);
                assignIfChanged(item->name, copy->name);
                assignIfChanged(item->sideText, sizeText(copy->sizeBytes));
                listed.emplace(*instanceId, std::move(item));
            }
            orphanItems = std::move(listed);
            refreshOrphanMembers();
            if (auto const* selected = std::get_if<Ids::InstanceId>(&selection.value());
                selected != nullptr && !orphanItems.contains(*selected))
                selectNothing();
        }

        void showEntryFields(WatchItem& item, SharedData::TrackedEntry const& entry)
        {
            assignIfChanged(item.entry, entry);
            assignIfChanged(item.name, entry.name);
            assignIfChanged(item.localPath, entry.localPath.toUtf8());
            assignIfChanged(item.remotePath, entry.remotePath.value());
            assignIfChanged(item.downloadedCopy, entry.downloadedCopy);
            assignIfChanged(item.isDirectory, entry.isDirectory);
            assignIfChanged(item.stop, entry.stop);
            assignIfChanged(item.stopText, entry.stop ? stopReasonText(texts, *entry.stop) : std::string{});
            assignIfChanged(*item.autoReupload, entry.options.autoReupload);
            assignIfChanged(*item.moveRemote, entry.options.moveRemoteOnLocalMove);
            assignIfChanged(*item.deleteRemote, entry.options.deleteRemoteOnLocalDelete);
            assignIfChanged(*item.checkRemote, entry.options.checkRemoteChanges);
        }

        /**
         * @brief Shows the file of @p row (nothing for a free row) in @p fileItem. @p watchedMoved recomputes the name
         *        below @p watched even when the row keeps its file.
         */
        static void showInFlightFile(
            InFlightFileItem& fileItem,
            std::optional<std::string> const& row,
            std::vector<std::string> const& keys,
            std::vector<SharedData::WatchFileProgress> const& files,
            Paths::LocalPath const& watched,
            bool watchedMoved
        )
        {
            const auto key = row ? std::ranges::find(keys, *row) : keys.end();
            if (key == keys.end())
            {
                assignIfChanged(fileItem.path, std::string{});
                assignIfChanged(fileItem.name, std::string{});
                assignIfChanged(*fileItem.totalBytes, std::int64_t{0});
                assignIfChanged(*fileItem.bytes, std::int64_t{0});
                return;
            }
            auto const& file = files[static_cast<std::size_t>(std::distance(keys.begin(), key))];
            if (fileItem.path.value() != *key || watchedMoved)
                assignIfChanged(fileItem.name, pathWithin(watched, file.path));
            assignIfChanged(fileItem.path, *key);
            assignIfChanged(*fileItem.totalBytes, static_cast<std::int64_t>(file.totalBytes));
            assignIfChanged(*fileItem.bytes, static_cast<std::int64_t>(file.bytes));
        }

        static void showInFlight(
            WatchRecord& watch,
            SharedData::WatchProgress const& progress,
            Paths::LocalPath const& watched,
            bool watchedMoved
        )
        {
            std::vector<std::string> keys{};
            keys.reserve(progress.inFlight.size());
            for (auto const& file : progress.inFlight)
                keys.push_back(file.path.toUtf8());
            watch.rows.update(keys);

            auto const& rows = watch.rows.rows();
            auto& inFlight = watch.item->inFlight;
            while (inFlight.value().size() > rows.size())
                inFlight.pop_back();
            for (std::size_t index = 0u; index < rows.size(); ++index)
            {
                const auto isNew = index >= inFlight.value().size();
                auto fileItem = isNew ? std::make_shared<InFlightFileItem>() : inFlight.value()[index];
                showInFlightFile(*fileItem, rows[index], keys, progress.inFlight, watched, watchedMoved);
                if (isNew)
                    inFlight.push_back(std::move(fileItem));
            }
        }

        void showProgress(WatchRecord& watch, SharedData::TrackedEntry const& entry, bool watchedMoved)
        {
            auto const* progress = model.progressOf(entry.watchId);
            if (progress == nullptr)
                return;
            auto& item = *watch.item;
            assignIfChanged(item.progress, *progress);
            assignIfChanged(*item.bytesTotal, static_cast<std::int64_t>(progress->bytesTotal));
            assignIfChanged(*item.bytesDone, static_cast<std::int64_t>(progress->bytesDone));
            showInFlight(watch, *progress, entry.localPath, watchedMoved);
        }

        void showConflicts(WatchItem& item, SharedData::TrackedEntry const& entry, bool watchedMoved)
        {
            syncRows(
                item.conflicts,
                model.conflictsOf(entry.watchId),
                &ConflictItem::conflict,
                [](SharedData::WatchConflict const& conflict)
                {
                    return conflict.conflictId;
                },
                [this, &entry](SharedData::WatchConflict const& conflict)
                {
                    return ConflictItem{
                        .conflict = conflict,
                        .file = pathWithin(entry.localPath, conflict.localPath),
                        .line = conflictLineText(texts, conflict),
                    };
                },
                watchedMoved
            );
            assignIfChanged(*item.conflictCount, model.conflictCountOf(entry.watchId));
        }

        void showFailures(WatchItem& item, SharedData::TrackedEntry const& entry, bool watchedMoved)
        {
            syncRows(
                item.failures,
                model.failuresOf(entry.watchId),
                &FailureItem::failure,
                [](SharedData::UploadFailure const& failure)
                {
                    return failure.failureId;
                },
                [this, &entry](SharedData::UploadFailure const& failure)
                {
                    return FailureItem{
                        .failure = failure,
                        .file = pathWithin(entry.localPath, failure.localPath),
                        .reason = failureReasonText(texts, failure.reason),
                        .time = localClockTime(failure.failedAt),
                    };
                },
                watchedMoved
            );
            assignIfChanged(*item.failureCount, model.failureCountOf(entry.watchId));
        }

        void showSummary(WatchItem& item)
        {
            assignIfChanged(item.status, model.statusOf(item.watchId));
            assignIfChanged(item.statusText, model.statusTextOf(item.watchId, texts));
            assignIfChanged(item.sideText, model.sideTextOf(item.watchId, texts));
            assignIfChanged(*item.ringFraction, model.ringFractionOf(item.watchId));
        }

        /**
         * @brief Updates @p parts of the item of @p watchId from the model, then its status and texts, and moves it
         *        to another group's list when its group changed. Does nothing for an unknown watch.
         */
        void refreshWatch(Ids::WatchId const& watchId, WatchParts parts)
        {
            const auto watch = watches.find(watchId);
            auto const* entry = model.entryOf(watchId);
            if (watch == watches.end() || entry == nullptr)
                return;
            auto& record = watch->second;
            auto& item = *record.item;

            const auto renamed = parts.entry && item.name.value() != entry->name;
            const auto watchedMoved = parts.entry && item.localPath.value() != entry->localPath.toUtf8();
            if (parts.entry)
                showEntryFields(item, *entry);
            if (parts.progress || watchedMoved)
                showProgress(record, *entry, watchedMoved);
            if (parts.conflicts || watchedMoved)
                showConflicts(item, *entry, watchedMoved);
            if (parts.failures || watchedMoved)
                showFailures(item, *entry, watchedMoved);
            showSummary(item);

            const auto group = model.groupOf(watchId);
            if (record.group != group)
            {
                const auto previous = std::exchange(record.group, group);
                if (previous)
                    refreshMembers(*previous);
                refreshMembers(group);
            }
            else if (renamed)
                refreshMembers(group);
        }
    };

    TrackingStore::TrackingStore(FileTrackingTexts texts)
        : impl_{std::make_unique<Implementation>(std::move(texts))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_MOVE(TrackingStore);

    void TrackingStore::showEntry(SharedData::TrackedEntry const& entry)
    {
        auto [watch, inserted] = impl_->watches.try_emplace(entry.watchId);
        if (inserted)
            watch->second.item = std::make_shared<WatchItem>(entry.watchId);
        const auto heldInstance = watch->second.item->entry.value().instanceId.has_value();

        impl_->model.showEntry(entry);
        impl_->refreshWatch(entry.watchId, inserted ? everyPart : WatchParts{.entry = true});
        if (heldInstance || entry.instanceId)
            impl_->refreshOrphans();
        impl_->notifyAttention();
    }

    void TrackingStore::removeEntry(Ids::WatchId const& watchId)
    {
        if (auto const* selected = std::get_if<Ids::WatchId>(&impl_->selection.value());
            selected != nullptr && *selected == watchId)
            impl_->selectNothing();

        std::optional<OutlinerGroup> group{};
        auto heldInstance = false;
        if (const auto watch = impl_->watches.find(watchId); watch != impl_->watches.end())
        {
            group = watch->second.group;
            heldInstance = watch->second.item->entry.value().instanceId.has_value();
            impl_->watches.erase(watch);
        }

        impl_->model.removeEntry(watchId);
        if (group)
            impl_->refreshMembers(*group);
        if (heldInstance)
            impl_->refreshOrphans();
        impl_->notifyAttention();
    }

    void TrackingStore::showConflict(SharedData::WatchConflict const& conflict)
    {
        const auto previous = impl_->model.watchOfConflict(conflict.conflictId);
        impl_->model.showConflict(conflict);
        impl_->refreshWatch(conflict.watchId, {.conflicts = true});
        if (previous && *previous != conflict.watchId)
            impl_->refreshWatch(*previous, {.conflicts = true});
        impl_->notifyAttention();
    }

    void TrackingStore::removeConflict(Ids::ConflictId const& conflictId)
    {
        const auto watchId = impl_->model.watchOfConflict(conflictId);
        impl_->model.removeConflict(conflictId);
        if (watchId)
            impl_->refreshWatch(*watchId, {.conflicts = true});
        impl_->notifyAttention();
    }

    void TrackingStore::showFailure(SharedData::UploadFailure const& failure)
    {
        const auto previous = impl_->model.watchOfFailure(failure.failureId);
        impl_->model.showFailure(failure);
        impl_->refreshWatch(failure.watchId, {.failures = true});
        if (previous && *previous != failure.watchId)
            impl_->refreshWatch(*previous, {.failures = true});
        impl_->notifyAttention();
    }

    void TrackingStore::removeFailure(Ids::FailureId const& failureId)
    {
        const auto watchId = impl_->model.watchOfFailure(failureId);
        impl_->model.removeFailure(failureId);
        if (watchId)
            impl_->refreshWatch(*watchId, {.failures = true});
        impl_->notifyAttention();
    }

    void TrackingStore::showWatchProgress(SharedData::WatchProgress const& progress)
    {
        impl_->model.showWatchProgress(progress);
        impl_->refreshWatch(progress.watchId, {.progress = true});
    }

    void TrackingStore::showOrphanedCopies(SharedData::OrphanedCopies const& copies)
    {
        impl_->model.showOrphanedCopies(copies);
        impl_->refreshOrphans();
    }

    void TrackingStore::clear()
    {
        impl_->selectNothing();
        impl_->model.clear();
        impl_->watches.clear();
        impl_->orphanItems.clear();
        impl_->refreshEveryMembers();
        impl_->notifyAttention();
    }

    TrackingModel const& TrackingStore::model() const
    {
        return impl_->model;
    }

    FileTrackingTexts const& TrackingStore::texts() const
    {
        return impl_->texts;
    }

    Nui::Observed<std::vector<std::shared_ptr<WatchItem>>>& TrackingStore::membersOf(OutlinerGroup group)
    {
        return impl_->members[indexOf(group)];
    }

    Nui::Observed<std::vector<std::shared_ptr<OrphanItem>>>& TrackingStore::orphanMembers()
    {
        return impl_->orphans;
    }

    std::shared_ptr<Nui::Observed<std::uint64_t>> TrackingStore::countOf(OutlinerGroup group)
    {
        return impl_->counts[indexOf(group)];
    }

    std::shared_ptr<WatchItem> TrackingStore::watchItem(Ids::WatchId const& watchId) const
    {
        const auto watch = impl_->watches.find(watchId);
        return watch != impl_->watches.end() ? watch->second.item : nullptr;
    }

    std::shared_ptr<OrphanItem> TrackingStore::orphanItem(Ids::InstanceId const& instanceId) const
    {
        const auto orphan = impl_->orphanItems.find(instanceId);
        return orphan != impl_->orphanItems.end() ? orphan->second : nullptr;
    }

    Nui::Observed<Selection>& TrackingStore::selection()
    {
        return impl_->selection;
    }

    void TrackingStore::select(Selection selection)
    {
        assignIfChanged(impl_->selection, std::move(selection));
    }

    void TrackingStore::moveSelection(SelectionMove move)
    {
        std::vector<Selection> shown{};
        for (const auto group : outlinerGroupOrder)
        {
            if (!impl_->expanded[indexOf(group)])
                continue;
            if (group == OutlinerGroup::Orphaned)
            {
                for (auto const& item : impl_->orphans.value())
                    shown.emplace_back(item->instanceId);
            }
            else
            {
                for (auto const& item : impl_->members[indexOf(group)].value())
                    shown.emplace_back(item->watchId);
            }
        }
        if (shown.empty())
            return;

        const auto last = shown.size() - 1u;
        const auto selected = std::ranges::find(shown, impl_->selection.value());
        const auto current = selected != shown.end()
            ? std::optional<std::size_t>{static_cast<std::size_t>(std::distance(shown.begin(), selected))}
            : std::nullopt;
        const auto target = [move, last, current]() -> std::size_t
        {
            switch (move)
            {
                case SelectionMove::First:
                    return 0u;
                case SelectionMove::Last:
                    return last;
                case SelectionMove::Next:
                    return current ? std::min(*current + 1u, last) : 0u;
                case SelectionMove::Previous:
                    break;
            }
            return current ? (*current == 0u ? 0u : *current - 1u) : last;
        }();
        select(shown[target]);
    }

    DetailTab TrackingStore::detailTab() const
    {
        return impl_->detailTab;
    }

    void TrackingStore::setDetailTab(DetailTab tab)
    {
        impl_->detailTab = tab;
        assignIfChanged(*impl_->detailTabKey, Utility::enumToString(tab));
    }

    std::shared_ptr<Nui::Observed<std::string>> TrackingStore::detailTabKey()
    {
        return impl_->detailTabKey;
    }

    Nui::Observed<std::string>& TrackingStore::filter()
    {
        return impl_->filter;
    }

    void TrackingStore::setFilter(std::string filter)
    {
        assignIfChanged(impl_->filter, filter);
        if (filter == impl_->appliedFilter)
            return;
        impl_->appliedFilter = std::move(filter);
        impl_->refreshEveryMembers();
    }

    bool TrackingStore::groupExpanded(OutlinerGroup group) const
    {
        return impl_->expanded[indexOf(group)];
    }

    void TrackingStore::setGroupExpanded(OutlinerGroup group, bool expanded)
    {
        impl_->expanded[indexOf(group)] = expanded;
    }

    bool TrackingStore::focusNewestConflict()
    {
        const auto newest = impl_->model.newestConflict();
        if (!newest)
            return false;
        select(newest->watchId);
        setDetailTab(DetailTab::Conflicts);
        return true;
    }

    void TrackingStore::onAttentionChanged(std::function<void(AttentionCounts const& counts)> onChanged)
    {
        impl_->onAttentionChanged = std::move(onChanged);
    }
}
