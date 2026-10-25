#pragma once

#include <backend/file_tracking/tracker_remote.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FileTracking
{
    /**
     * @brief A TrackerRemote that also queues many uploads or deletes as one operation.
     */
    class BulkTrackerRemote : public TrackerRemote
    {
      public:
        /**
         * @brief Called once on the strand for each entry of a bulk once the bulk ended, with the entry's index.
         */
        using OnEntryEnded = std::function<void(std::size_t index, SharedData::OperationCompletionReason reason)>;

        /**
         * @brief Called once on the strand for each upload of a bulk once the bulk ended, with the entry's index.
         */
        using OnUploadEntryEnded = std::function<void(std::size_t index, UploadEnd const& ended)>;

        /**
         * @brief Called on the strand while an upload of a bulk is on its way, with the entry's index, never after the
         *        bulk ended.
         */
        using OnUploadEntryProgress = std::function<void(std::size_t index, UploadProgress const& progress)>;

        /**
         * @brief Queues the uploads of @p requests, regular files that may create the directories above their
         *        destination, as one operation. The index of an entry is that of its request. @p onProgress may be
         *        empty.
         */
        virtual std::expected<void, std::string> uploadAll(
            std::vector<UploadRequest> const& requests,
            OnUploadEntryEnded onEnded,
            OnUploadEntryProgress onProgress
        ) = 0;

        /**
         * @brief Queues the deletes of @p paths as one operation, a directory only when it is empty.
         */
        virtual std::expected<void, std::string>
        removeAll(std::vector<Paths::RemotePath> const& paths, OnEntryEnded onEnded) = 0;
    };

    /**
     * @brief Hands the uploads and deletes a Tracker queues within a short time to the server as one bulk each, which
     *        costs one operation, one card and one listing refresh instead of one per path. Everything else passes
     *        through at once. The tracker orders its operations per path, so what is gathered here lies on unrelated
     *        paths and may run side by side. Lives on the strand of the session.
     */
    class BatchingRemote : public TrackerRemote
    {
      public:
        struct Limits
        {
            /** @brief A batch goes once nothing was added to it for this long. */
            std::chrono::milliseconds quiet{100};
            /** @brief A batch goes at the latest this long after its first entry. */
            std::chrono::milliseconds longest{1000};
            /** @brief A batch goes at once when it holds this many entries. */
            std::size_t maxEntries{500};
            /** @brief A file this large goes on its own, an upload that can be stopped, see cancelUpload(). */
            std::uint64_t aloneFromBytes{8u * 1024u * 1024u};
        };

        BatchingRemote(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::unique_ptr<BulkTrackerRemote> inner,
            Limits limits
        );
        BatchingRemote(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::unique_ptr<BulkTrackerRemote> inner
        );
        ~BatchingRemote() override;
        BatchingRemote(BatchingRemote const&) = delete;
        BatchingRemote& operator=(BatchingRemote const&) = delete;
        BatchingRemote(BatchingRemote&&) = delete;
        BatchingRemote& operator=(BatchingRemote&&) = delete;

        std::expected<std::optional<ServerState>, std::string> stat(Paths::RemotePath const& path) override;
        std::expected<std::optional<ServerState>, std::string> linkStat(Paths::RemotePath const& path) override;
        std::expected<void, std::string> download(
            Paths::RemotePath const& source,
            Paths::LocalPath const& destination,
            std::uint64_t sizeBytes,
            OnEnded onEnded
        ) override;

        /**
         * @brief Gathers @p request into the next batch. It goes on its own when it creates no missing directories,
         *        refreshes the listing, is no regular file, is a large one, continues a partial file or is alone in
         *        its batch. @p onProgress hears what the inner remote tells of it.
         */
        std::expected<void, std::string>
        upload(UploadRequest const& request, OnUploadEnded onEnded, OnUploadProgress onProgress) override;

        /**
         * @brief An upload still gathered ends as cancelled without being sent, one on its way is stopped by the inner
         *        remote. One in a bulk goes on.
         */
        void cancelUpload(Paths::RemotePath const& destination, PartialFile partial) override;

        std::expected<void, std::string>
        discardPartialUpload(Paths::RemotePath const& destination, OnEnded onEnded) override;

        std::expected<void, std::string>
        rename(Paths::RemotePath const& source, Paths::RemotePath const& destination, OnEnded onEnded) override;

        /**
         * @brief Gathers the delete of @p path into the next batch, alone in it it goes on its own.
         */
        std::expected<void, std::string> remove(Paths::RemotePath const& path, OnEnded onEnded) override;

      private:
        struct Implementation;
        std::shared_ptr<Implementation> impl_;
    };
}
