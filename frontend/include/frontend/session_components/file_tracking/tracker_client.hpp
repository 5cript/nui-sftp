#pragma once

#include <shared_data/file_tracking/ended_watches.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_options.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace FileTracking
{
    class TrackingStore;

    /**
     * @brief Talks to the tracker of a session, which lives in the backend as "FileTracker::<sessionId>". Registers
     *        every event and feeds it to the TrackingStore, syncing once per burst of events from a task of its own;
     *        has one method per RPC. Owns no panel state. A refusal of a fire-and-forget RPC is only logged.
     */
    class TrackerClient
    {
      public:
        /**
         * @param store Fed by the events, outlives the client.
         * @param onEnded Gets the watches that ended without the user stopping them (onEntriesEnded), for the toasts.
         */
        TrackerClient(TrackingStore& store, std::function<void(SharedData::EndedWatches const& ended)> onEnded);
        ROAR_PIMPL_SPECIAL_FUNCTIONS_NO_MOVE(TrackerClient);

        /**
         * @brief Follows the tracker of @p sessionId from now on: deactivates first, registers onEntry, onEntryRemoved,
         *        onEntriesEnded, onConflict, onConflictsRemoved, onFailure, onFailuresRemoved, onWatchProgress and
         *        onOrphanedCopies, then asks for the orphan list.
         */
        void activate(Ids::SessionId sessionId);

        /**
         * @brief Unregisters every event and clears the store, the session ended. Does not sync.
         */
        void deactivate();

        /**
         * @brief Asks the tracker to download @p remotePath into a directory of its own, and to watch it there when
         *        @p watch is set. @p onOpened gets the copy, @p onRefused the translated reason when there is none.
         *        Neither is called when the download did not end well, its card in the queue tells why.
         */
        void open(
            Paths::RemotePath const& remotePath,
            std::uint64_t sizeBytes,
            bool watch,
            std::function<void(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)> onOpened,
            std::function<void(std::string const& reason)> onRefused
        );

        /**
         * @brief Asks the tracker whether @p item could be uploaded into @p directory and watched now, before the user
         *        is asked anything about the upload. @p onWatchable gets the item canonical, as the watch will report
         *        it; @p onRefused gets the translated reason.
         */
        void checkUploadAndWatch(
            Paths::LocalPath const& item,
            Paths::RemotePath const& directory,
            std::function<void(Paths::LocalPath const& canonicalItem)> onWatchable,
            std::function<void(std::string const& reason)> onRefused
        );

        /**
         * @brief Asks the tracker to watch @p item, upload it to @p remotePath and mirror its changes from then on.
         *        @p onRefused gets the translated reason when it does not.
         */
        void uploadAndWatch(
            Paths::LocalPath const& item,
            Paths::RemotePath const& remotePath,
            std::function<void(std::string const& reason)> onRefused
        );

        /**
         * @brief Asks the tracker to remove @p localPath, a download of the instance @p instanceId the user decided not
         *        to open, with its watch.
         */
        void discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath);

        /**
         * @brief Stops the watch @p watchId (stop), it stays listed as stopped by the user.
         */
        void stop(Ids::WatchId const& watchId);

        /**
         * @brief Turns @p option of @p watchId on or off (setOption).
         */
        void setOption(Ids::WatchId const& watchId, SharedData::WatchOption option, bool enabled);

        /**
         * @brief Answers the conflict @p conflictId with @p resolution (answerConflict).
         */
        void answerConflict(Ids::ConflictId const& conflictId, SharedData::ConflictResolution resolution);

        /**
         * @brief Answers every conflict of @p watchId with @p resolution (answerAllConflicts).
         */
        void answerAllConflicts(Ids::WatchId const& watchId, SharedData::ConflictResolution resolution);

        /**
         * @brief Uploads the file of the failure @p failureId again (retryFailure).
         */
        void retryFailure(Ids::FailureId const& failureId);

        /**
         * @brief Forgets the failure @p failureId without uploading (dismissFailure).
         */
        void dismissFailure(Ids::FailureId const& failureId);

        /**
         * @brief Uploads the files of every failure of @p watchId again (retryAllFailures).
         */
        void retryAllFailures(Ids::WatchId const& watchId);

        /**
         * @brief Forgets every failure of @p watchId (dismissAllFailures).
         */
        void dismissAllFailures(Ids::WatchId const& watchId);

        /**
         * @brief Drops the stopped watch @p watchId from the list (removeStopped); a stopped copy becomes an orphan.
         */
        void removeStopped(Ids::WatchId const& watchId);

        /**
         * @brief Drops every stopped watch from the list (removeAllStopped).
         */
        void removeAllStopped();

        /**
         * @brief Asks for the orphaned copies (listOrphanedCopies), the list arrives as onOrphanedCopies.
         */
        void listOrphanedCopies();

        /**
         * @brief Deletes the orphaned copy @p instanceId (removeOrphanedCopy). @p onFailed gets the backend's error
         *        when it refused or failed.
         */
        void
        removeOrphanedCopy(Ids::InstanceId const& instanceId, std::function<void(std::string const& error)> onFailed);

        /**
         * @brief Deletes every orphaned copy (removeAllOrphanedCopies).
         */
        void removeAllOrphanedCopies();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
