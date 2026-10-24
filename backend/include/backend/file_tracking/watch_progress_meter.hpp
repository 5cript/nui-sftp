#pragma once

#include <backend/file_tracking/tracker_remote.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>

#include <cstdint>
#include <map>
#include <optional>

namespace FileTracking
{
    /**
     * @brief Measures how far one watch is with its changes, see SharedData::WatchProgress. The tracker tells it how
     *        many changes are on their way and how its uploads go, by the local path each upload started with. A
     *        burst lasts until nothing is on its way, finishBurst() then keeps it as the last sync.
     *
     * Files that an upload still on its way finished count as done, the others it started as running, every other
     * pending change as waiting. A completed upload counts its bytes as done; one that did not complete keeps the
     * bytes it sent and its other bytes leave the total, so the bar still reaches its end.
     */
    class WatchProgressMeter
    {
      public:
        /**
         * @brief How an upload ended, see uploadEnded().
         */
        enum class UploadResult
        {
            Completed,
            /** @brief Failed, cancelled or never queued. */
            NotCompleted,
        };

        explicit WatchProgressMeter(Ids::WatchId watchId);

        Ids::WatchId const& watchId() const;

        /**
         * @brief The changes on their way: waiting for the check, an answer or the queue, or running. The files that
         *        uploads on their way finished are among them until those uploads ended.
         */
        void setChangesPending(std::uint64_t count);

        /**
         * @brief The pending changes that wait for the user's answer to a conflict.
         */
        void setWaitingForAnswer(std::uint64_t count);

        /**
         * @brief @p count changes of the current burst ended, however they went.
         */
        void changesEnded(std::uint64_t count);

        /**
         * @brief The upload of @p path was handed to the queue, @p sizeBytes is its size when known. An upload of
         *        @p path still on its way counts as ended without completing.
         */
        void uploadQueued(Paths::LocalPath const& path, std::optional<std::uint64_t> sizeBytes = std::nullopt);

        /**
         * @brief The upload of @p path is as far as @p progress, it started on the server. Nothing for an upload
         *        that is not on its way.
         */
        void uploadProgressed(Paths::LocalPath const& path, UploadProgress const& progress);

        /**
         * @brief The upload of @p path ended. End it together with changesEnded() and setChangesPending() for its
         *        changes, the files it finished count as pending again in between.
         */
        void uploadEnded(Paths::LocalPath const& path, UploadResult result);

        /**
         * @brief Forgets what is on its way, for a watch that stops: the uploads count as ended without completing,
         *        no change is pending or waits for an answer.
         */
        void dropPending();

        /**
         * @brief Whether nothing is on its way: no pending change, none waiting for an answer, no upload.
         */
        bool isIdle() const;

        /**
         * @brief Keeps the current burst as the last sync, ended at @p endedAt (seconds since the Unix epoch), and
         *        starts the next one. Nothing happens while something is on its way, nor for a burst in which nothing
         *        ended and nothing was sent.
         * @return Whether it kept the burst.
         */
        bool finishBurst(std::uint64_t endedAt);

        SharedData::WatchProgress snapshot() const;

        /**
         * @brief Whether the snapshot changed since takeDirty() was called last.
         */
        bool isDirty() const;

        /**
         * @brief Whether the snapshot changed since this was called last, see isDirty().
         */
        bool takeDirty();

      private:
        struct ActiveUpload
        {
            std::optional<std::uint64_t> sizeBytes{};
            /** @brief The last progress, nothing before the upload started on the server. */
            std::optional<UploadProgress> progress{};
        };

        void endUpload(ActiveUpload const& upload, UploadResult result);

      private:
        Ids::WatchId watchId_;
        std::uint64_t changesPending_{0u};
        std::uint64_t waitingForAnswer_{0u};
        /** @brief The changes of the current burst that ended. */
        std::uint64_t burstChangesDone_{0u};
        /** @brief The bytes the ended uploads of the current burst count as, both done and total. */
        std::uint64_t burstBytes_{0u};
        std::map<Paths::LocalPath, ActiveUpload> uploads_{};
        std::optional<SharedData::WatchSync> lastSync_{};
        bool dirty_{false};
    };
}
