#include <backend/file_tracking/batching_remote.hpp>
#include <log/log.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace FileTracking
{
    namespace
    {
        struct GatheredUpload
        {
            UploadRequest request;
            TrackerRemote::OnUploadEnded onEnded;
            TrackerRemote::OnUploadProgress onProgress;
        };

        struct GatheredRemoval
        {
            Paths::RemotePath path;
            TrackerRemote::OnEnded onEnded;
        };

        /**
         * @brief Whether @p request may go in a bulk: a regular file smaller than @p aloneFromBytes. Symbolic links
         *        and directories keep the single upload that handles them, and so does what must not create the
         *        directories above it, refreshes or continues a partial file. A large file goes alone, so it can be
         *        stopped.
         */
        bool fitsBulk(UploadRequest const& request, std::uint64_t aloneFromBytes)
        {
            if (!request.createMissingDirectories || request.refreshListing || request.continuesPartialOf)
                return false;
            std::error_code statusError;
            if (std::filesystem::symlink_status(request.source.native(), statusError).type() !=
                std::filesystem::file_type::regular)
                return false;
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(request.source.native(), sizeError);
            return !sizeError && size < aloneFromBytes;
        }

        /**
         * @brief Ends every one of @p onEnded with @p ended.
         */
        template <typename CallbackT, typename EndT>
        void endAll(std::vector<CallbackT> const& onEnded, EndT const& ended)
        {
            for (auto const& callback : onEnded)
                if (callback)
                    callback(ended);
        }

        /**
         * @brief Hands what a bulk tells of each entry, its end or its progress, to the callback it was gathered with.
         */
        template <typename CallbackT>
        auto perEntry(std::vector<CallbackT> callbacks)
        {
            return [callbacks = std::move(callbacks)](std::size_t index, auto const& told)
            {
                if (index < callbacks.size() && callbacks[index])
                    callbacks[index](told);
            };
        }

        /**
         * @brief How an upload the inner remote refused with @p error ends: as cancelled when its source is gone,
         *        whose deletion is mirrored apart, otherwise as failed for not being queued.
         */
        UploadEnd refusedUpload(UploadRequest const& request, std::string const& error)
        {
            std::error_code statusError;
            if (std::filesystem::symlink_status(request.source.native(), statusError).type() ==
                std::filesystem::file_type::not_found)
                return {.reason = SharedData::OperationCompletionReason::Canceled};
            return {
                .reason = SharedData::OperationCompletionReason::Failed,
                .failure = UploadFailureInfo{.reason = SharedData::UploadFailureReason::NotQueued, .detail = error},
            };
        }
    }

    struct BatchingRemote::Implementation : public std::enable_shared_from_this<BatchingRemote::Implementation>
    {
        std::unique_ptr<BulkTrackerRemote> inner;
        Limits limits;
        boost::asio::steady_timer timer;
        std::vector<GatheredUpload> uploads{};
        std::vector<GatheredRemoval> removals{};
        std::chrono::steady_clock::time_point batchStart{};

        Implementation(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::unique_ptr<BulkTrackerRemote> inner,
            Limits limits
        )
            : inner{std::move(inner)}
            , limits{limits}
            , timer{std::move(strand)}
        {}

        /**
         * @brief Something was gathered: the batch goes once nothing more comes for the quiet time, after the longest
         *        time or right after this call when it is full.
         */
        void gathered()
        {
            const auto now = std::chrono::steady_clock::now();
            const auto count = uploads.size() + removals.size();
            if (count >= limits.maxEntries)
            {
                timer.cancel();
                boost::asio::post(
                    timer.get_executor(),
                    [weak = weak_from_this(),
                        full = std::exchange(uploads, {}),
                        fullRemovals = std::exchange(removals, {})]() mutable
                    {
                        if (const auto self = weak.lock())
                            self->send(std::move(full), std::move(fullRemovals));
                    }
                );
                return;
            }
            if (count == 1)
                batchStart = now;
            timer.expires_at(std::min(now + limits.quiet, batchStart + limits.longest));
            timer.async_wait(
                [weak = weak_from_this()](boost::system::error_code const& error)
                {
                    if (error)
                        return;
                    if (const auto self = weak.lock())
                        self->send(std::exchange(self->uploads, {}), std::exchange(self->removals, {}));
                }
            );
        }

        void send(std::vector<GatheredUpload> gatheredUploads, std::vector<GatheredRemoval> gatheredRemovals)
        {
            sendUploads(std::move(gatheredUploads));
            sendRemovals(std::move(gatheredRemovals));
        }

        void sendUploads(std::vector<GatheredUpload> gathered)
        {
            std::vector<UploadRequest> bulk;
            std::vector<OnUploadEnded> bulkEnds;
            std::vector<OnUploadProgress> bulkProgress;
            std::vector<GatheredUpload> single;
            for (auto& upload : gathered)
            {
                if (fitsBulk(upload.request, limits.aloneFromBytes))
                {
                    bulk.push_back(std::move(upload.request));
                    bulkEnds.push_back(std::move(upload.onEnded));
                    bulkProgress.push_back(std::move(upload.onProgress));
                }
                else
                    single.push_back(std::move(upload));
            }
            if (bulk.size() == 1)
                single.push_back({
                    .request = std::move(bulk.front()),
                    .onEnded = std::move(bulkEnds.front()),
                    .onProgress = std::move(bulkProgress.front()),
                });
            else if (!bulk.empty())
            {
                Log::info("FileTracking: uploading {} changed files as one operation", bulk.size());
                if (const auto queued = inner->uploadAll(bulk, perEntry(bulkEnds), perEntry(std::move(bulkProgress)));
                    !queued)
                {
                    Log::warn("FileTracking: did not upload {} changed files: {}", bulk.size(), queued.error());
                    for (std::size_t index = 0; index < bulk.size(); ++index)
                        if (bulkEnds[index])
                            bulkEnds[index](refusedUpload(bulk[index], queued.error()));
                }
            }
            for (auto& upload : single)
            {
                if (const auto queued = inner->upload(upload.request, upload.onEnded, std::move(upload.onProgress));
                    !queued)
                {
                    // A temporary file an editor already renamed or removed ends up here.
                    Log::warn("FileTracking: did not upload '{}': {}", upload.request.source.toUtf8(), queued.error());
                    if (upload.onEnded)
                        upload.onEnded(refusedUpload(upload.request, queued.error()));
                }
            }
        }

        void sendRemovals(std::vector<GatheredRemoval> gathered)
        {
            if (gathered.empty())
                return;
            if (gathered.size() == 1)
            {
                auto& removal = gathered.front();
                if (const auto queued = inner->remove(removal.path, removal.onEnded); !queued)
                {
                    Log::error("FileTracking: did not delete '{}': {}", removal.path.value(), queued.error());
                    if (removal.onEnded)
                        removal.onEnded(SharedData::OperationCompletionReason::Failed);
                }
                return;
            }
            std::vector<Paths::RemotePath> paths;
            std::vector<OnEnded> ends;
            for (auto& removal : gathered)
            {
                paths.push_back(std::move(removal.path));
                ends.push_back(std::move(removal.onEnded));
            }
            Log::info("FileTracking: deleting {} paths as one operation", paths.size());
            if (const auto queued = inner->removeAll(paths, perEntry(ends)); !queued)
            {
                Log::error("FileTracking: did not delete {} paths: {}", paths.size(), queued.error());
                endAll(ends, SharedData::OperationCompletionReason::Failed);
            }
        }

        /**
         * @brief Takes the gathered upload to @p destination out of the next batch, nothing when none is gathered.
         */
        std::optional<GatheredUpload> takeGathered(Paths::RemotePath const& destination)
        {
            const auto found = std::ranges::find_if(
                uploads,
                [&destination](GatheredUpload const& upload)
                {
                    return upload.request.destination == destination;
                }
            );
            if (found == uploads.end())
                return std::nullopt;
            auto taken = std::move(*found);
            uploads.erase(found);
            return taken;
        }
    };

    BatchingRemote::BatchingRemote(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        std::unique_ptr<BulkTrackerRemote> inner,
        Limits limits
    )
        : impl_{std::make_shared<Implementation>(std::move(strand), std::move(inner), limits)}
    {}

    BatchingRemote::BatchingRemote(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        std::unique_ptr<BulkTrackerRemote> inner
    )
        : BatchingRemote{std::move(strand), std::move(inner), Limits{}}
    {}

    BatchingRemote::~BatchingRemote() = default;

    std::expected<std::optional<ServerState>, std::string> BatchingRemote::stat(Paths::RemotePath const& path)
    {
        return impl_->inner->stat(path);
    }

    std::expected<std::optional<ServerState>, std::string> BatchingRemote::linkStat(Paths::RemotePath const& path)
    {
        return impl_->inner->linkStat(path);
    }

    std::expected<void, std::string> BatchingRemote::download(
        Paths::RemotePath const& source,
        Paths::LocalPath const& destination,
        std::uint64_t sizeBytes,
        OnEnded onEnded
    )
    {
        return impl_->inner->download(source, destination, sizeBytes, std::move(onEnded));
    }

    std::expected<void, std::string>
    BatchingRemote::upload(UploadRequest const& request, OnUploadEnded onEnded, OnUploadProgress onProgress)
    {
        impl_->uploads.push_back(
            {.request = request, .onEnded = std::move(onEnded), .onProgress = std::move(onProgress)}
        );
        impl_->gathered();
        return {};
    }

    void BatchingRemote::cancelUpload(Paths::RemotePath const& destination, PartialFile partial)
    {
        auto gathered = impl_->takeGathered(destination);
        if (!gathered)
            return impl_->inner->cancelUpload(destination, partial);
        Log::debug("FileTracking: not sending the upload to '{}', it was stopped", destination.value());
        // Ends after the call, whose caller may still be deciding what follows the stop.
        boost::asio::post(
            impl_->timer.get_executor(),
            [onEnded = std::move(gathered->onEnded)]()
            {
                if (onEnded)
                    onEnded({.reason = SharedData::OperationCompletionReason::Canceled});
            }
        );
    }

    std::expected<void, std::string>
    BatchingRemote::discardPartialUpload(Paths::RemotePath const& destination, OnEnded onEnded)
    {
        return impl_->inner->discardPartialUpload(destination, std::move(onEnded));
    }

    std::expected<void, std::string>
    BatchingRemote::rename(Paths::RemotePath const& source, Paths::RemotePath const& destination, OnEnded onEnded)
    {
        return impl_->inner->rename(source, destination, std::move(onEnded));
    }

    std::expected<void, std::string> BatchingRemote::remove(Paths::RemotePath const& path, OnEnded onEnded)
    {
        impl_->removals.push_back({.path = path, .onEnded = std::move(onEnded)});
        impl_->gathered();
        return {};
    }
}
