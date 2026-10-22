#pragma once

#include <backend/file_tracking/disk_place.hpp>
#include <backend/file_tracking/local_item_watch.hpp>
#include <backend/file_tracking/reported_change.hpp>
#include <ids/ids.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace FileTracking
{
    namespace LocalWatchManagerDetail
    {
        /**
         * @brief A canonical path and where it lies on disk.
         */
        struct PlacedPath
        {
            std::filesystem::path path;
            DiskPlace place;

            /**
             * @brief Looks @p path up on disk.
             */
            PlacedPath(std::filesystem::path path);
            PlacedPath(std::filesystem::path path, DiskPlace place);
        };

        /**
         * @brief Whether @p first and @p second overlap: they are the same, or one lies below the other. Case is
         *        ignored on Windows and macOS, whose file systems fold it, and paths that exist are compared on disk
         *        too, which tells the same file under another spelling anywhere.
         */
        bool overlaps(PlacedPath const& first, PlacedPath const& second);

        /**
         * @brief Refuses @p item when it overlaps (see overlaps()) one of @p watchedItems or
         *        @p temporaryDownloadsDirectory, whose downloads Open and Watch watches. Watches never nest, so no
         *        change has two owners. All paths canonical.
         * @return An error built with Utility::localizedMessage.
         */
        std::expected<void, std::string> refuseOverlap(
            PlacedPath const& item,
            std::vector<PlacedPath> const& watchedItems,
            PlacedPath const& temporaryDownloadsDirectory
        );
    }

    /**
     * @brief Owns the LocalItemWatch objects of files and directories the user uploaded and watches ("Upload and
     *        Watch"). No two watches overlap, and none overlaps the temporary downloads directory that the
     *        TempDirInstanceManager watches. Lives on its strand: every member must be called from it.
     */
    class LocalWatchManager
    {
      public:
        /**
         * @param strand Runs the changes of the watches.
         * @param temporaryDownloadsDirectory Where the TempDirInstanceManager keeps its instances, no local watch
         *        may overlap it.
         */
        LocalWatchManager(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::filesystem::path temporaryDownloadsDirectory
        );
        ~LocalWatchManager() = default;
        LocalWatchManager(LocalWatchManager const&) = delete;
        LocalWatchManager& operator=(LocalWatchManager const&) = delete;
        LocalWatchManager(LocalWatchManager&&) = delete;
        LocalWatchManager& operator=(LocalWatchManager&&) = delete;

        /**
         * @brief Whether @p item can be watched now: it exists, is not a filesystem root and overlaps no watch.
         * @return The canonical item, an error built with Utility::localizedMessage that says why it cannot be watched.
         */
        std::expected<std::filesystem::path, std::string> checkWatchable(std::filesystem::path const& item) const;

        /**
         * @brief Starts watching @p item as @p watchId, its changes go to @p sink.
         * @return The new watch, an error when @p item cannot be watched or overlaps another watch.
         */
        std::expected<LocalItemWatch const*, std::string> watch(
            Ids::WatchId const& watchId,
            std::filesystem::path const& item,
            std::string const& downloadTempSuffix,
            ChangeSink sink
        );

        /**
         * @brief Ends the watch @p watchId, nothing happens when there is none.
         */
        void unwatch(Ids::WatchId const& watchId);

        LocalItemWatch const* find(Ids::WatchId const& watchId) const;

      private:
        /**
         * @brief Refuses the canonical @p item when it overlaps a watch other than @p ignoredWatchId or the temporary
         *        downloads directory, see LocalWatchManagerDetail::refuseOverlap.
         */
        std::expected<void, std::string> refuseOverlapWithWatches(
            std::filesystem::path const& item,
            std::optional<Ids::WatchId> const& ignoredWatchId = std::nullopt
        ) const;

      private:
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        std::filesystem::path temporaryDownloadsDirectory_;
        std::map<Ids::WatchId, LocalItemWatch> watches_;
    };
}
