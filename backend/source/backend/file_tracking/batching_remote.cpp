#include <backend/file_tracking/batching_remote.hpp>
#include <log/log.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <filesystem>
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
        };

        struct GatheredRemoval
        {
            Paths::RemotePath path;
            TrackerRemote::OnEnded onEnded;
        };

        /**
         * @brief Whether @p request may go in a bulk: a regular file, symbolic links and directories keep the single
         *        upload that handles them, and so does what must not create the directories above it or refreshes.
         */
        bool fitsBulk(UploadRequest const& request)
        {
            if (!request.createMissingDirectories || request.refreshListing)
                return false;
            std::error_code error;
            return std::filesystem::symlink_status(request.source.native(), error).type() ==
                std::filesystem::file_type::regular;
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
         * @brief Hands the end of each entry of a bulk to the callback it was gathered with.
         */
        template <typename CallbackT>
        auto entryEndsOf(std::vector<CallbackT> onEnded)
        {
            return [onEnded = std::move(onEnded)](std::size_t index, auto const& ended)
            {
                if (index < onEnded.size() && onEnded[index])
                    onEnded[index](ended);
            };
        }

        const UploadEnd failedUpload{.reason = SharedData::OperationCompletionReason::Failed};
    }

    struct BatchingRemote::Implementation : public std::enable_shared_from_this<BatchingRemote::Implementation>
    {
        std::unique_ptr<BulkTrackerRemote> inner;
        Timing timing;
        boost::asio::steady_timer timer;
        std::vector<GatheredUpload> uploads{};
        std::vector<GatheredRemoval> removals{};
        std::chrono::steady_clock::time_point batchStart{};

        Implementation(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::unique_ptr<BulkTrackerRemote> inner,
            Timing timing
        )
            : inner{std::move(inner)}
            , timing{timing}
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
            if (count >= timing.maxEntries)
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
            timer.expires_at(std::min(now + timing.quiet, batchStart + timing.longest));
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
            std::vector<GatheredUpload> single;
            for (auto& upload : gathered)
            {
                if (fitsBulk(upload.request))
                {
                    bulk.push_back(std::move(upload.request));
                    bulkEnds.push_back(std::move(upload.onEnded));
                }
                else
                    single.push_back(std::move(upload));
            }
            if (bulk.size() == 1)
                single.push_back({.request = std::move(bulk.front()), .onEnded = std::move(bulkEnds.front())});
            else if (!bulk.empty())
            {
                Log::info("FileTracking: uploading {} changed files as one operation", bulk.size());
                if (const auto queued = inner->uploadAll(bulk, entryEndsOf(bulkEnds)); !queued)
                {
                    Log::warn("FileTracking: did not upload {} changed files: {}", bulk.size(), queued.error());
                    endAll(bulkEnds, failedUpload);
                }
            }
            for (auto& upload : single)
            {
                if (const auto queued = inner->upload(upload.request, upload.onEnded); !queued)
                {
                    // A temporary file an editor already renamed or removed ends up here.
                    Log::warn("FileTracking: did not upload '{}': {}", upload.request.source.toUtf8(), queued.error());
                    if (upload.onEnded)
                        upload.onEnded(failedUpload);
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
            if (const auto queued = inner->removeAll(paths, entryEndsOf(ends)); !queued)
            {
                Log::error("FileTracking: did not delete {} paths: {}", paths.size(), queued.error());
                endAll(ends, SharedData::OperationCompletionReason::Failed);
            }
        }
    };

    BatchingRemote::BatchingRemote(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        std::unique_ptr<BulkTrackerRemote> inner,
        Timing timing
    )
        : impl_{std::make_shared<Implementation>(std::move(strand), std::move(inner), timing)}
    {}

    BatchingRemote::BatchingRemote(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        std::unique_ptr<BulkTrackerRemote> inner
    )
        : BatchingRemote{std::move(strand), std::move(inner), Timing{}}
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

    std::expected<void, std::string> BatchingRemote::upload(UploadRequest const& request, OnUploadEnded onEnded)
    {
        impl_->uploads.push_back({.request = request, .onEnded = std::move(onEnded)});
        impl_->gathered();
        return {};
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
