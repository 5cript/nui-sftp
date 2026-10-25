#pragma once

#include <backend/file_tracking/tracker_remote.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/transfer_progress.hpp>
#include <paths/paths.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace FileTracking
{
    /**
     * @brief Turns the successive progress the operation queue tells of one bulk or directory upload into the
     *        progress of each upload it carries, by the index of its request. An upload is told when its progress
     *        changed, from the moment it started on its way or ended.
     */
    class BulkEntryProgress
    {
      public:
        /**
         * @brief An upload of a bulk of files. Its index is that of its request, which is its entry index in the
         *        bulk.
         */
        struct File
        {
            Paths::LocalPath source;
            /** @brief Its size when it was queued, until its transfer tells it. */
            std::uint64_t sizeBytes{0};
        };

        /**
         * @brief The progress of the upload at @p index.
         */
        struct EntryProgress
        {
            std::size_t index{0};
            UploadProgress progress{};
        };

        /**
         * @brief A bulk of @p files.
         */
        static BulkEntryProgress ofFiles(std::vector<File> files);

        /**
         * @brief The upload of the directory @p source to @p destination, the single upload at index 0. Its files
         *        and bytes are the entries below it the bulk is through with, the left out ones too, as the queue
         *        counts them.
         */
        static BulkEntryProgress ofDirectory(Paths::LocalPath source, Paths::RemotePath destination);

        /**
         * @brief The uploads whose progress changed with @p progress, the next snapshot of the bulk, by index.
         */
        std::vector<EntryProgress> update(SharedData::BulkProgress const& progress);

      private:
        struct Entry
        {
            File file;
            /** @brief Seen on its way, it is done once it is no longer. */
            bool started{false};
            /** @brief Listed among the bulk's failed entries. */
            bool leftOut{false};
            /** @brief The last update that saw it on its way, 0 for none. */
            std::uint64_t onItsWayIn{0u};
            std::optional<UploadProgress> told{};
        };

        struct Directory
        {
            Paths::LocalPath source;
            Paths::RemotePath destination;
            std::optional<UploadProgress> told{};
        };

        BulkEntryProgress(std::vector<Entry> entries, std::optional<Directory> directory);

        std::vector<EntryProgress> updateFiles(SharedData::BulkProgress const& progress);
        std::vector<EntryProgress> updateDirectory(SharedData::BulkProgress const& progress);

      private:
        std::vector<Entry> entries_;
        std::optional<Directory> directory_;
        /** @brief The entries seen on their way that were not told done yet, by index. */
        std::vector<std::size_t> started_{};
        std::uint64_t updates_{0u};
    };

    /**
     * @brief How far the single upload of the file or link @p source is, its transfer between its minimum and
     *        maximum at @p progress.
     */
    UploadProgress uploadProgressOf(SharedData::TransferProgress const& progress, Paths::LocalPath const& source);

    /**
     * @brief Whether @p left and @p right tell the same.
     */
    bool sameProgress(UploadProgress const& left, UploadProgress const& right);
}
