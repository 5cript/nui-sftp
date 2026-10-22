#pragma once

#include <backend/file_tracking/disk_place.hpp>
#include <backend/file_tracking/reported_change.hpp>
#include <ids/ids.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace FileTracking
{
    struct DirectoryChange;

    /**
     * @brief How long after the watch followed a rename the item appearing at its old name takes the watch back: an
     *        editor that moves the original aside and writes the new file later than the settle time.
     */
    inline constexpr std::chrono::milliseconds renameReturnTime{5000};

    /**
     * @brief Watches a file or directory the user has locally, not a download in a temporary directory: its
     *        directory without recursion, so atomic saves, renames and the removal of the item show, and the
     *        item itself recursively when it is a directory. Only changes of the item reach its sink.
     *
     * The watch follows the item: a rename within its directory is held back for the settle time and then
     * reported and followed, unless the item comes back meanwhile (editors that move the original aside to write
     * a new one). When the item appears at its old name within renameReturnTime after that, the watch goes back and
     * reports a rename back and the new content. The removal of the item is held back for the settle time too, a
     * rename of it or the item coming back meanwhile replaces it. A rename onto a path it may not follow, or onto a
     * temporary name (see isTransientFile), is reported as the removal of the item. A directory that is created anew
     * at the item is watched again. Lives on its strand: every member must be called from it.
     */
    class LocalItemWatch
    {
      public:
        /**
         * @param watchId Names the watch.
         * @param item The file or directory to watch, canonical, see LocalWatchManager::checkWatchable.
         * @param sink Receives the changes of the item on @p strand.
         * @param downloadTempSuffix The suffix downloads write under until complete, see
         *        Persistence::effectiveTempFileSuffix.
         * @param mayFollow Whether the watch, named by its watchId(), may follow a rename of the item to the path,
         *        false when that overlaps another watch. Called on the strand.
         */
        static std::expected<LocalItemWatch, std::string> open(
            Ids::WatchId watchId,
            std::filesystem::path const& item,
            boost::asio::strand<boost::asio::any_io_executor> strand,
            ChangeSink sink,
            std::string const& downloadTempSuffix,
            std::function<bool(Ids::WatchId const& watchId, std::filesystem::path const& renamed)> mayFollow
        );

        ~LocalItemWatch();
        LocalItemWatch(LocalItemWatch const&) = delete;
        LocalItemWatch& operator=(LocalItemWatch const&) = delete;
        LocalItemWatch(LocalItemWatch&&) noexcept;
        LocalItemWatch& operator=(LocalItemWatch&&) noexcept;

        Ids::WatchId const& watchId() const;

        /** @brief The watched item, canonical like the paths of the change events. */
        std::filesystem::path const& item() const;

        /** @brief Where item() lies on disk, taken when the watch started or followed or found it anew. */
        DiskPlace const& place() const;

        /** @brief The directory of item(), canonical. */
        std::filesystem::path directory() const;

        bool isDirectory() const;

        /**
         * @brief Handles @p change as if a watched directory reported it. Tests use it for event orders their
         *        platform does not produce, like Windows reporting a rename as a removal first.
         */
        void handleChange(DirectoryChange const& change);

      private:
        struct State;
        explicit LocalItemWatch(std::shared_ptr<State> state);

      private:
        std::shared_ptr<State> state_;
    };
}
