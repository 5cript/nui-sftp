#pragma once

#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>
#include <shared_data/file_tracking/watch_stop.hpp>
#include <ids/ids.hpp>

#include <nui/event_system/observed_value.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FileTracking
{
    /**
     * @brief One row of the files in flight of a watch. A free row (between taken ones) has an empty path and name.
     */
    struct InFlightFileItem
    {
        /** @brief The local path, the value of data-ft-progress-file. */
        Nui::Observed<std::string> path{};
        /** @brief The path below the watched item, shown above the bar. */
        Nui::Observed<std::string> name{};
        std::shared_ptr<Nui::Observed<std::int64_t>> bytes{std::make_shared<Nui::Observed<std::int64_t>>(0)};
        std::shared_ptr<Nui::Observed<std::int64_t>> totalBytes{std::make_shared<Nui::Observed<std::int64_t>>(0)};
    };

    /**
     * @brief One row of the Conflicts tab. Replaced as a whole when the conflict changes.
     */
    struct ConflictItem
    {
        SharedData::WatchConflict conflict{};
        /** @brief The path below the watched item, the file name for a watched file. */
        std::string file{};
        /** @brief "Changed on the server at 14:21, saved here at 14:27". */
        std::string line{};
    };

    /**
     * @brief One row of the Failed tab. Replaced as a whole when the failure changes.
     */
    struct FailureItem
    {
        SharedData::UploadFailure failure{};
        /** @brief The path below the watched item, the file name for a watched file. */
        std::string file{};
        /** @brief The reason sentence. */
        std::string reason{};
        /** @brief The clock time of failedAt. */
        std::string time{};
    };

    /**
     * @brief What the views show of one watch. The store owns it and keeps it while the watch is known, so a view may
     *        capture its shared pointer. Every member is updated in place, never replaced.
     */
    struct WatchItem
    {
        explicit WatchItem(Ids::WatchId watchId)
            : watchId{std::move(watchId)}
        {}

        Ids::WatchId watchId;
        /** @brief The entry as the tracker sent it last. */
        Nui::Observed<SharedData::TrackedEntry> entry{};
        Nui::Observed<std::string> name{};
        /** @brief The watched local item, for a copy its file in the instance directory. */
        Nui::Observed<std::string> localPath{};
        Nui::Observed<std::string> remotePath{};
        /** @brief An Open and Watch copy rather than an uploaded original. */
        Nui::Observed<bool> downloadedCopy{false};
        Nui::Observed<bool> isDirectory{false};
        Nui::Observed<WatchStatus> status{WatchStatus::Watching};
        /** @brief The status badge text: a count, Syncing, Up to date or Stopped. */
        Nui::Observed<std::string> statusText{};
        /** @brief The outliner side text: a count or the short stop reason, empty otherwise. */
        Nui::Observed<std::string> sideText{};
        /** @brief The fraction of the outliner ring while syncing, 0 to 1. */
        std::shared_ptr<Nui::Observed<double>> ringFraction{std::make_shared<Nui::Observed<double>>(0.0)};
        /** @brief Set once the watch stopped. */
        Nui::Observed<std::optional<SharedData::WatchStop>> stop{};
        /** @brief The sentence of the stop reason strip, empty while running. */
        Nui::Observed<std::string> stopText{};

        std::shared_ptr<Nui::Observed<bool>> autoReupload{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> moveRemote{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> deleteRemote{std::make_shared<Nui::Observed<bool>>(false)};
        std::shared_ptr<Nui::Observed<bool>> checkRemote{std::make_shared<Nui::Observed<bool>>(false)};

        /** @brief The last progress the tracker sent, zero until one arrives. */
        Nui::Observed<SharedData::WatchProgress> progress{};
        /** @brief bytesDone of the progress, for the total bar. */
        std::shared_ptr<Nui::Observed<std::int64_t>> bytesDone{std::make_shared<Nui::Observed<std::int64_t>>(0)};
        /** @brief bytesTotal of the progress, for the total bar. */
        std::shared_ptr<Nui::Observed<std::int64_t>> bytesTotal{std::make_shared<Nui::Observed<std::int64_t>>(0)};
        /** @brief One row per file in flight, laid out by InFlightRows so rows do not shift. */
        Nui::Observed<std::vector<std::shared_ptr<InFlightFileItem>>> inFlight{};

        /** @brief The conflicts of the page shown, newest first, as TrackingModel::conflictsOf orders them. */
        Nui::Observed<std::vector<std::shared_ptr<ConflictItem>>> conflicts{};
        /** @brief The failures of the page shown, newest first, as TrackingModel::failuresOf orders them. */
        Nui::Observed<std::vector<std::shared_ptr<FailureItem>>> failures{};
        /** @brief Every conflict of the watch, not only those of the page shown. */
        std::shared_ptr<Nui::Observed<std::uint64_t>> conflictCount{std::make_shared<Nui::Observed<std::uint64_t>>(0u)};
        /** @brief Every failure of the watch, not only those of the page shown. */
        std::shared_ptr<Nui::Observed<std::uint64_t>> failureCount{std::make_shared<Nui::Observed<std::uint64_t>>(0u)};
        /** @brief The page of the Conflicts tab shown, 0-based. */
        std::shared_ptr<Nui::Observed<int>> conflictPage{std::make_shared<Nui::Observed<int>>(0)};
        /** @brief The pages of the Conflicts tab, at least 1. */
        std::shared_ptr<Nui::Observed<int>> conflictPageCount{std::make_shared<Nui::Observed<int>>(1)};
        /** @brief The page of the Failed tab shown, 0-based. */
        std::shared_ptr<Nui::Observed<int>> failurePage{std::make_shared<Nui::Observed<int>>(0)};
        /** @brief The pages of the Failed tab, at least 1. */
        std::shared_ptr<Nui::Observed<int>> failurePageCount{std::make_shared<Nui::Observed<int>>(1)};
    };

    /**
     * @brief What the views show of one orphaned copy. The store owns it while the copy is listed.
     */
    struct OrphanItem
    {
        explicit OrphanItem(Ids::InstanceId instanceId)
            : instanceId{std::move(instanceId)}
        {}

        Ids::InstanceId instanceId;
        /** @brief The copy as the tracker listed it last. */
        Nui::Observed<SharedData::OrphanedCopy> copy{};
        Nui::Observed<std::string> name{};
        /** @brief Its size, the outliner side text. */
        Nui::Observed<std::string> sideText{};
    };
}
