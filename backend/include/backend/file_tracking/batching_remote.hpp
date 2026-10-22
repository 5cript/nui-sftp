#pragma once

#include <backend/file_tracking/tracker_remote.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <cstddef>
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
         * @brief Queues the uploads of @p requests, regular files that may create the directories above their
         *        destination, as one operation.
         */
        virtual std::expected<void, std::string>
        uploadAll(std::vector<UploadRequest> const& requests, OnUploadEntryEnded onEnded) = 0;

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
        struct Timing
        {
            /** @brief A batch goes once nothing was added to it for this long. */
            std::chrono::milliseconds quiet{100};
            /** @brief A batch goes at the latest this long after its first entry. */
            std::chrono::milliseconds longest{1000};
            /** @brief A batch goes at once when it holds this many entries. */
            std::size_t maxEntries{500};
        };

        BatchingRemote(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::unique_ptr<BulkTrackerRemote> inner,
            Timing timing
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
         *        refreshes the listing, is no regular file or is alone in its batch.
         */
        std::expected<void, std::string> upload(UploadRequest const& request, OnUploadEnded onEnded) override;

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
