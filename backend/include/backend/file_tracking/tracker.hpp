#pragma once

#include <backend/file_tracking/tracker_remote.hpp>
#include <backend/file_tracking/tracker_view.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/server_changed_question.hpp>
#include <shared_data/file_tracking/watch_options.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace FileTracking
{
    class LocalWatchManager;

    /**
     * @brief What the trackers of the sessions work with, both managers on the strand of the sessions.
     */
    struct TrackerServices
    {
        TempDirInstanceManager* instances{nullptr};
        LocalWatchManager* localWatches{nullptr};
        /** @brief The options a new watch starts with, called on the strand. */
        std::function<SharedData::WatchOptions()> defaultOptions{};
    };

    /**
     * @brief Organizes the file tracking of one session: holds its watches, decides what each change of a watched
     *        item does on the server and queues that, and asks the user when the server's copy changed meanwhile.
     *        Its view shows the outcome, the frontend only forwards the user's input. No two watches upload to the
     *        same remote path or one below the other. Lives on the strand of the session: every member must be called
     *        from it.
     *
     * @par Accepted limitation on macOS
     *      A directory that leaves with its content keeps its server copy, a move to the trash looks the same. A
     *      directory deleted entry by entry reports its content first and its server copy goes. FSEvents reports
     *      nothing below the watched directory itself when it is removed with its content (`rm -rf`), so its whole
     *      server tree stays and a directory created anew in its place is uploaded into that tree. See
     *      TrackerTest.TheWatchedDirectoryRemovedWithItsContentKeepsItsServerTreeOnlyOnMacOs.
     */
    class Tracker
    {
      public:
        /**
         * @brief A downloaded copy, ready to be opened, see open().
         */
        struct OpenedCopy
        {
            Ids::InstanceId instanceId;
            Paths::LocalPath localPath;
        };

        /**
         * @brief Why open() has no copy: a message built with Utility::localizedMessage, nothing when the download
         *        did not end well, which its card in the queue shows.
         */
        using OpenFailure = std::optional<std::string>;

        /**
         * @brief Where uploadAndWatch() would upload an item to, see checkUploadAndWatch().
         */
        struct UploadTarget
        {
            /** @brief Canonical, as the watch reports it. */
            Paths::LocalPath item;
            Paths::RemotePath destination;
        };

        /**
         * @param strand The strand of the session, the changes of the watches are handled on it.
         * @param instances Holds the instances of the downloaded copies, on @p strand. Outlives the tracker.
         * @param localWatches Holds the watches of the uploaded originals, on @p strand. Outlives the tracker.
         * @param downloadTempSuffix The suffix the session's downloads write under until complete, see
         *        Persistence::effectiveTempFileSuffix. The watches ignore those files.
         * @param defaultOptions The options a new watch starts with.
         */
        Tracker(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            TempDirInstanceManager& instances,
            LocalWatchManager& localWatches,
            std::unique_ptr<TrackerRemote> remote,
            std::unique_ptr<TrackerView> view,
            std::string downloadTempSuffix,
            std::function<SharedData::WatchOptions()> defaultOptions
        );
        ~Tracker();
        Tracker(Tracker const&) = delete;
        Tracker& operator=(Tracker const&) = delete;
        Tracker(Tracker&&) = delete;
        Tracker& operator=(Tracker&&) = delete;

        /**
         * @brief Downloads @p remotePath into an instance directory of its own, and watches it there when @p watch
         *        is set: its changes go back to @p remotePath from then on. @p onOpened gets the copy once the watch
         *        started, or why there is none. A watch that would overlap another one on the server is refused
         *        before anything is downloaded.
         */
        void open(
            Paths::RemotePath const& remotePath,
            std::uint64_t sizeBytes,
            bool watch,
            std::function<void(std::expected<OpenedCopy, OpenFailure> const&)> onOpened
        );

        /**
         * @brief Where uploadAndWatch() would upload @p item when it goes into @p directory: under the name of the
         *        canonical item, so a symbolic link goes under its target's name.
         * @return Why it cannot be watched now, built with Utility::localizedMessage: it overlaps another watch
         *         locally or on the server, or cannot be watched at all.
         */
        std::expected<UploadTarget, std::string>
        checkUploadAndWatch(Paths::LocalPath const& item, Paths::RemotePath const& directory) const;

        /**
         * @brief Watches @p item, a canonical local file or directory, uploads it to @p destination, which may be
         *        replaced, and mirrors its changes from then on.
         * @return Why it cannot be watched, see checkUploadAndWatch(). Nothing is uploaded then.
         */
        std::expected<void, std::string>
        uploadAndWatch(Paths::LocalPath const& item, Paths::RemotePath const& destination);

        /**
         * @brief Ends the watch @p watchId: a downloaded copy's instance ends with it, an uploaded original stays.
         */
        void stop(Ids::WatchId const& watchId);

        void setOption(Ids::WatchId const& watchId, SharedData::WatchOption option, bool value);

        /**
         * @brief Removes @p path, a download of the instance @p instanceId the user decided not to open, and ends
         *        its watch when it has one. Once nothing else is left in the instance, it ends and its directory is
         *        removed, so it leaves no orphan.
         */
        std::expected<DiscardOutcome, std::string>
        discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& path);

        /**
         * @brief The user's answer to the question @p questionId, see TrackerView::ask().
         */
        void answer(Ids::QuestionId const& questionId, SharedData::ServerChangedAnswer answer);

        /**
         * @brief Ends every watch without telling the view, for a frontend that is gone or a session that closes. The
         *        instances of the copies it downloaded, watched or only opened, end too: they count as orphaned then,
         *        or go when nothing is left in them. A download on its way is dropped, it opens nothing.
         */
        void stopAll();

        /**
         * @brief The session lost its connection: every watch ends like in stopAll(), either side may change until it
         *        is back, and the view shows why. The user watches anew.
         */
        void connectionLost();

      private:
        struct Implementation;
        std::shared_ptr<Implementation> impl_;
    };
}
