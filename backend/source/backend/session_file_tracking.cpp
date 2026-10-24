#include <backend/session.hpp>
#include <backend/file_tracking/batching_remote.hpp>
#include <backend/file_tracking/frontend_tracker_view.hpp>
#include <backend/file_tracking/tracker.hpp>
#include <backend/rpc_path_parameters.hpp>
#include <constants/sftp.hpp>
#include <shared_data/file_operations/bulk_delete_request.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <log/log.hpp>

#include <boost/asio/post.hpp>
#include <fmt/format.h>
#include <libssh/sftp.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    /**
     * @brief Hands each of the @p count entries of a bulk that ended as @p completed its end, an entry @p skipped
     *        tells of failed.
     */
    void endEntries(
        SharedData::OperationCompleted const& completed,
        std::size_t count,
        std::function<bool(std::size_t)> const& skipped,
        FileTracking::BulkTrackerRemote::OnEntryEnded const& onEnded
    )
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto reason = completed.reason == SharedData::OperationCompletionReason::Completed && skipped(index)
                ? SharedData::OperationCompletionReason::Failed
                : completed.reason;
            onEnded(index, reason);
        }
    }

    /**
     * @brief What the server has at uploaded destinations, asked for on the strand of an SFTP channel. Its delivery
     *        gets the states on the session strand once they are known, or without them when the channel drops the
     *        request unasked.
     */
    class DestinationStates
    {
      public:
        using States = std::vector<std::optional<FileTracking::ServerState>>;
        using Delivery = std::function<void(States const& states)>;

        DestinationStates(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::vector<Paths::RemotePath> paths,
            Delivery delivery
        )
            : strand_{std::move(strand)}
            , paths_{std::move(paths)}
            , states_(paths_.size())
            , delivery_{std::move(delivery)}
        {}
        ~DestinationStates()
        {
            deliver();
        }
        DestinationStates(DestinationStates const&) = delete;
        DestinationStates& operator=(DestinationStates const&) = delete;
        DestinationStates(DestinationStates&&) = delete;
        DestinationStates& operator=(DestinationStates&&) = delete;

        void askInStrand(SecureShell::SftpSession& channel)
        {
            for (std::size_t index = 0; index < paths_.size(); ++index)
            {
                if (const auto information = channel.lstatInStrand(paths_[index]))
                    states_[index] = FileTracking::ServerState{.mtime = information->mtime, .size = information->size};
            }
            deliver();
        }

      private:
        void deliver()
        {
            if (!delivery_)
                return;
            boost::asio::post(
                strand_,
                [states = std::exchange(states_, {}), delivery = std::exchange(delivery_, {})]()
                {
                    delivery(states);
                }
            );
        }

      private:
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        std::vector<Paths::RemotePath> paths_;
        States states_;
        Delivery delivery_;
    };

    /**
     * @brief Hands only the reason of a completed operation to @p onEnded, nothing for an empty one.
     */
    std::function<void(SharedData::OperationCompleted const&)> reasonTo(FileTracking::TrackerRemote::OnEnded onEnded)
    {
        if (!onEnded)
            return {};
        return [onEnded = std::move(onEnded)](SharedData::OperationCompleted const& completed)
        {
            onEnded(completed.reason);
        };
    }

    /**
     * @brief What an upload must find at its destination to replace it, from what the tracker expects there. A file
     *        the server lost counts as changed, the tracker asks before it recreates it.
     */
    std::optional<SharedData::ComparedState>
    comparedStateOf(std::optional<FileTracking::ExpectedDestination> const& expected)
    {
        if (!expected)
            return std::nullopt;
        if (!expected->file)
            return SharedData::ComparedState{};
        return SharedData::ComparedState{
            .exists = true,
            .type = SharedData::FileType::Regular,
            .size = expected->file->size,
            .mtime = expected->file->mtime,
            .mustStillExist = true,
        };
    }

    bool isDirectoryLocally(Paths::LocalPath const& path)
    {
        std::error_code statusError;
        return std::filesystem::is_directory(std::filesystem::symlink_status(path.native(), statusError));
    }

    /**
     * @brief Gathers the files an upload sent unchanged, told on the strands of its transfers, until it ended.
     */
    class SentFiles : public std::enable_shared_from_this<SentFiles>
    {
      public:
        UploadOperation::OnSent recorder()
        {
            return [self = shared_from_this()](
                       Paths::LocalPath const& source, std::optional<UploadOperation::SentSource> const& unchanged
                   )
            {
                const auto state = unchanged.transform(
                    [](UploadOperation::SentSource const& sent)
                    {
                        return FileTracking::SourceState{.size = sent.size, .modified = sent.modified};
                    }
                );
                std::scoped_lock lock{self->mutex_};
                self->files_.push_back({.path = source, .state = state});
            };
        }

        std::vector<FileTracking::SentFile> take()
        {
            std::scoped_lock lock{mutex_};
            return std::exchange(files_, {});
        }

      private:
        std::mutex mutex_{};
        std::vector<FileTracking::SentFile> files_{};
    };
}

class Session::FileTrackingRemote : public FileTracking::BulkTrackerRemote
{
  public:
    explicit FileTrackingRemote(std::weak_ptr<Session> session)
        : session_{std::move(session)}
    {}

    std::expected<std::optional<FileTracking::ServerState>, std::string> stat(Paths::RemotePath const& path) override
    {
        return stateOf(path, /*followLinks=*/true);
    }

    std::expected<std::optional<FileTracking::ServerState>, std::string>
    linkStat(Paths::RemotePath const& path) override
    {
        return stateOf(path, /*followLinks=*/false);
    }

    std::expected<void, std::string> download(
        Paths::RemotePath const& source,
        Paths::LocalPath const& destination,
        std::uint64_t sizeBytes,
        OnEnded onEnded
    ) override
    {
        return queue(
            [&](OperationQueue& queue, SecureShell::SftpSession& channel, Ids::OperationId const& operationId)
            {
                return queue.addDownloadOperation(
                    channel,
                    operationId,
                    destination,
                    source,
                    /*allowOverwrite=*/false,
                    /*isBigFile=*/sizeBytes > Constants::bigFileCutOff,
                    /*insertRefresh=*/false,
                    /*createMissingDirectories=*/false,
                    SharedData::OperationMode::PriorityQueued
                );
            },
            reasonTo(std::move(onEnded))
        );
    }

    std::expected<void, std::string> upload(
        FileTracking::UploadRequest const& request,
        OnUploadEnded onEnded,
        [[maybe_unused]] OnUploadProgress onProgress
    ) override
    {
        const bool isDirectory = isDirectoryLocally(request.source);
        const auto sent = std::make_shared<SentFiles>();
        std::optional<Ids::OperationId> queuedAs{};
        auto queued = queue(
            [&request, &sent, &queuedAs](
                OperationQueue& queue, SecureShell::SftpSession& channel, Ids::OperationId const& operationId
            )
            {
                auto added = queue.addUploadOperation(
                    channel,
                    operationId,
                    request.source,
                    request.destination,
                    /*allowOverwrite=*/true,
                    /*isBigFile=*/false,
                    request.refreshListing,
                    request.createMissingDirectories,
                    SharedData::OperationMode::PriorityQueued,
                    /*sourceMayVanish=*/true,
                    comparedStateOf(request.expectedDestination),
                    sent->recorder(),
                    request.continuesPartialOf
                );
                // The tracker asks the user about a server copy that changed meanwhile.
                if (added)
                {
                    queue.leaveChangedDestinationsToOwner(operationId);
                    queuedAs = operationId;
                }
                return added;
            },
            forgettingSentUpload(
                request.destination, endOfUpload(session_, request.destination, isDirectory, sent, std::move(onEnded))
            )
        );
        if (queued && queuedAs)
            sentUploads_->insert_or_assign(request.destination.value(), *queuedAs);
        return queued;
    }

    void cancelUpload(Paths::RemotePath const& destination, FileTracking::PartialFile partial) override
    {
        const auto found = sentUploads_->find(destination.value());
        const auto session = session_.lock();
        if (found == sentUploads_->end() || !session)
            return;
        Log::info("FileTracking: stopping the upload to '{}'", destination.value());
        // The stopped upload ends after the tracker's call, which may still decide what follows it.
        boost::asio::post(
            *session->strand_,
            [weak = session_,
                operationId = found->second,
                partialFile = partial == FileTracking::PartialFile::Kept ? UploadOperation::PartialFile::Kept
                                                                          : UploadOperation::PartialFile::Discarded]()
            {
                if (const auto locked = weak.lock(); locked && locked->operationQueue_)
                    locked->operationQueue_->cancelUpload(operationId, partialFile);
            }
        );
    }

    std::expected<void, std::string>
    discardPartialUpload(Paths::RemotePath const& destination, OnEnded onEnded) override
    {
        return withChannel(
            [&destination,
                &onEnded](Session& session, SecureShell::SftpSession& channel) -> std::expected<void, std::string>
            {
                session.operationQueue_->discardPartialUpload(
                    channel,
                    destination,
                    [onEnded = std::move(onEnded)]()
                    {
                        if (onEnded)
                            onEnded(SharedData::OperationCompletionReason::Completed);
                    }
                );
                return {};
            }
        );
    }

    std::expected<void, std::string>
    rename(Paths::RemotePath const& source, Paths::RemotePath const& destination, OnEnded onEnded) override
    {
        return queue(
            [&](OperationQueue& queue, SecureShell::SftpSession& channel, Ids::OperationId const& operationId)
            {
                return queue.addRenameOperation(
                    channel, operationId, source, destination, SharedData::OperationMode::PriorityQueued
                );
            },
            reasonTo(std::move(onEnded))
        );
    }

    std::expected<void, std::string> remove(Paths::RemotePath const& path, OnEnded onEnded) override
    {
        return queue(
            [&path](OperationQueue& queue, SecureShell::SftpSession& channel, Ids::OperationId const& operationId)
            {
                return queue.addDeleteOperation(
                    channel,
                    operationId,
                    path,
                    /*recursive=*/false,
                    /*insertRefresh=*/true,
                    SharedData::OperationMode::PriorityQueued
                );
            },
            reasonTo(std::move(onEnded))
        );
    }

    std::expected<void, std::string> uploadAll(
        std::vector<FileTracking::UploadRequest> const& requests,
        OnUploadEntryEnded onEnded,
        [[maybe_unused]] OnUploadEntryProgress onProgress
    ) override
    {
        return withChannel(
            [&requests,
                &onEnded](Session& session, SecureShell::SftpSession& channel) -> std::expected<void, std::string>
            {
                const auto sent = std::make_shared<SentFiles>();
                SharedData::BulkUploadRequest bulk{
                    .allowOverwrite = true,
                    .mode = SharedData::OperationMode::PriorityQueued,
                };
                bulk.entries.reserve(requests.size());
                for (auto const& request : requests)
                {
                    std::error_code sizeError;
                    const auto sizeBytes = std::filesystem::file_size(request.source.native(), sizeError);
                    bulk.entries.push_back({
                        .source = request.source,
                        .destination = request.destination,
                        .sizeBytes = sizeError ? 0u : static_cast<std::uint64_t>(sizeBytes),
                        .mayOverwrite = true,
                        .comparedDestination = comparedStateOf(request.expectedDestination),
                    });
                }
                auto& queue = *session.operationQueue_;
                const auto cardId = Ids::generateOperationId();
                queue.addBulkUploadOperation(
                    channel,
                    std::move(bulk),
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    cardId,
                    /*sourceMayVanish=*/true,
                    sent->recorder()
                );
                // The tracker decides anew what goes once the session is back.
                queue.discardResumeBackup(cardId);
                queue.leaveChangedDestinationsToOwner(cardId);
                queue.whenCompleted(
                    cardId,
                    [session = session.weak_from_this(),
                        onEnded = std::move(onEnded),
                        sent,
                        sources = requests | std::views::transform(&FileTracking::UploadRequest::source) |
                            std::ranges::to<std::vector>(),
                        destinations = requests | std::views::transform(&FileTracking::UploadRequest::destination) |
                            std::ranges::to<std::vector>()](SharedData::OperationCompleted const& completed)
                    {
                        endUploads(session, completed, sources, destinations, sent->take(), onEnded);
                    }
                );
                session.resetQueueThrottle();
                return {};
            }
        );
    }

    std::expected<void, std::string>
    removeAll(std::vector<Paths::RemotePath> const& paths, OnEntryEnded onEnded) override
    {
        return withChannel(
            [&paths, &onEnded](Session& session, SecureShell::SftpSession& channel) -> std::expected<void, std::string>
            {
                SharedData::BulkDeleteRequest bulk{
                    .insertRefresh = true,
                    .mode = SharedData::OperationMode::PriorityQueued,
                    .listedContentOnly = true,
                };
                bulk.entries.reserve(paths.size());
                for (auto const& path : paths)
                    bulk.entries.push_back({.target = path});
                auto& queue = *session.operationQueue_;
                const auto cardId = Ids::generateOperationId();
                if (queue.addBulkDeleteOperation(channel, bulk, cardId) == 0)
                    return std::unexpected{std::string{"The server refused every delete"}};
                queue.discardResumeBackup(cardId);
                queue.whenCompleted(
                    cardId,
                    [onEnded = std::move(onEnded), paths](SharedData::OperationCompleted const& completed)
                    {
                        const auto failed = completed.failedEntries | std::views::keys | std::ranges::to<std::set>();
                        endEntries(
                            completed,
                            paths.size(),
                            [&failed, &paths](std::size_t index)
                            {
                                return failed.contains(paths[index].value());
                            },
                            onEnded
                        );
                    }
                );
                session.resetQueueThrottle();
                return {};
            }
        );
    }

  private:
    /**
     * @brief Hands the end of an upload to @p onEnded, a completed one of anything but a directory once the server
     *        told what it has at @p destination, with the files @p sent tells of. Nothing for an empty @p onEnded.
     */
    static std::function<void(SharedData::OperationCompleted const&)> endOfUpload(
        std::weak_ptr<Session> session,
        Paths::RemotePath destination,
        bool isDirectory,
        std::shared_ptr<SentFiles> sent,
        OnUploadEnded onEnded
    )
    {
        if (!onEnded)
            return {};
        return [session = std::move(session),
                   destination = std::move(destination),
                   isDirectory,
                   sent = std::move(sent),
                   onEnded = std::move(onEnded)](SharedData::OperationCompleted const& completed)
        {
            const bool uploaded = completed.reason == SharedData::OperationCompletionReason::Completed;
            if (!uploaded || isDirectory)
                return onEnded({
                    .reason = completed.reason,
                    .changedOnServer =
                        completed.error && completed.error->type == SharedData::OperationErrorType::ChangedSinceCompare,
                    .sentFiles = uploaded ? sent->take() : std::vector<FileTracking::SentFile>{},
                });
            askStatesThen(
                session,
                {destination},
                [onEnded, sentFiles = sent->take()](DestinationStates::States const& states)
                {
                    onEnded({
                        .reason = SharedData::OperationCompletionReason::Completed,
                        .destinationState = states.front(),
                        .sentFiles = sentFiles,
                    });
                }
            );
        };
    }

    /**
     * @brief Hands each upload of a bulk that ended as @p completed its end: a completed one once the server told
     *        what it has at its destination, of @p destinations by index, with its source when it is among @p sent.
     */
    static void endUploads(
        std::weak_ptr<Session> const& session,
        SharedData::OperationCompleted const& completed,
        std::vector<Paths::LocalPath> const& sources,
        std::vector<Paths::RemotePath> const& destinations,
        std::vector<FileTracking::SentFile> const& sent,
        OnUploadEntryEnded const& onEnded
    )
    {
        if (completed.reason != SharedData::OperationCompletionReason::Completed)
        {
            for (std::size_t index = 0; index < destinations.size(); ++index)
                onEnded(index, {.reason = completed.reason});
            return;
        }
        const auto skipped = completed.indexedFailures |
            std::views::transform(
                [](SharedData::IndexedFailure const& failure)
                {
                    return std::pair{static_cast<std::size_t>(failure.index), failure.type};
                }
            ) |
            std::ranges::to<std::map>();
        std::vector<std::size_t> uploaded;
        std::vector<Paths::RemotePath> uploadedDestinations;
        for (std::size_t index = 0; index < destinations.size(); ++index)
        {
            if (const auto failure = skipped.find(index); failure != skipped.end())
                onEnded(
                    index,
                    {
                        .reason = SharedData::OperationCompletionReason::Failed,
                        .changedOnServer = failure->second == SharedData::OperationErrorType::ChangedSinceCompare,
                    }
                );
            else
            {
                uploaded.push_back(index);
                uploadedDestinations.push_back(destinations[index]);
            }
        }
        if (uploaded.empty())
            return;
        const auto sentBySource = sent |
            std::views::transform(
                [](FileTracking::SentFile const& file)
                {
                    return std::pair{file.path.toUtf8(), file};
                }
            ) |
            std::ranges::to<std::map>();
        const auto sentFilesOf = [&sentBySource, &sources](std::size_t index)
        {
            const auto found = sentBySource.find(sources[index].toUtf8());
            if (found == sentBySource.end())
                return std::vector<FileTracking::SentFile>{};
            return std::vector{found->second};
        };
        auto uploadedSent = uploaded | std::views::transform(sentFilesOf) | std::ranges::to<std::vector>();
        askStatesThen(
            session,
            std::move(uploadedDestinations),
            [onEnded,
                uploaded = std::move(uploaded),
                uploadedSent = std::move(uploadedSent)](DestinationStates::States const& states)
            {
                for (std::size_t position = 0; position < uploaded.size(); ++position)
                    onEnded(
                        uploaded[position],
                        {
                            .reason = SharedData::OperationCompletionReason::Completed,
                            .destinationState = states[position],
                            .sentFiles = uploadedSent[position],
                        }
                    );
            }
        );
    }

    /**
     * @brief Asks the server what it has at @p paths on the strand of one of its SFTP channels, so the session strand
     *        that runs the queue does not wait for it. @p delivery gets the states on the session strand.
     */
    static void askStatesThen(
        std::weak_ptr<Session> const& session,
        std::vector<Paths::RemotePath> paths,
        DestinationStates::Delivery delivery
    )
    {
        const auto locked = session.lock();
        if (!locked)
            return delivery(DestinationStates::States(paths.size()));
        auto states = std::make_shared<DestinationStates>(*locked->strand_, std::move(paths), std::move(delivery));
        // Without a channel the states go unasked once they are dropped here.
        if (const auto channel = locked->anySftpChannel())
            channel->perform(
                [states, weakChannel = std::weak_ptr{channel}]()
                {
                    if (const auto channel = weakChannel.lock())
                        states->askInStrand(*channel);
                }
            );
    }

    /**
     * @brief What the server has at @p path, a symbolic link followed when @p followLinks is set.
     */
    std::expected<std::optional<FileTracking::ServerState>, std::string>
    stateOf(Paths::RemotePath const& path, bool followLinks)
    {
        return withChannel(
            [&path, followLinks](
                Session&, SecureShell::SftpSession& channel
            ) -> std::expected<std::optional<FileTracking::ServerState>, std::string>
            {
                auto future = followLinks ? channel.stat(path) : channel.lstat(path);
                if (future.wait_for(Session::futureTimeout) != std::future_status::ready)
                    return std::unexpected{fmt::format("Asking the server about '{}' timed out", path.value())};
                const auto result = future.get();
                if (!result)
                {
                    if (result.error().sftpError == SSH_FX_NO_SUCH_FILE)
                        return std::nullopt;
                    return std::unexpected{result.error().toString()};
                }
                return FileTracking::ServerState{.mtime = result->mtime, .size = result->size};
            }
        );
    }

    /**
     * @brief Calls @p function with the session and one of its SFTP channels, an error when it has none.
     */
    template <typename FunctionT>
    auto withChannel(FunctionT&& function) const -> std::invoke_result_t<FunctionT, Session&, SecureShell::SftpSession&>
    {
        const auto session = session_.lock();
        if (!session || !session->operationQueue_)
            return std::unexpected{std::string{"The session has ended"}};
        const auto channel = session->anySftpChannel();
        if (!channel)
            return std::unexpected{std::string{"The session has no SFTP channel"}};
        return std::forward<FunctionT>(function)(*session, *channel);
    }

    /**
     * @brief Queues an operation with @p add under a new id without a resume backup, @p onCompleted hears when it
     *        ended.
     */
    template <typename AddT>
    std::expected<void, std::string>
    queue(AddT&& add, std::function<void(SharedData::OperationCompleted const&)> onCompleted)
    {
        return withChannel(
            [&add,
                &onCompleted](Session& session, SecureShell::SftpSession& channel) -> std::expected<void, std::string>
            {
                const auto operationId = Ids::generateOperationId();
                const auto added = add(*session.operationQueue_, channel, operationId);
                if (!added)
                    return std::unexpected{added.error().toString()};
                // Like the bulks: the watches end with the connection, a reconnect queues nothing of theirs again.
                session.operationQueue_->discardResumeBackup(operationId);
                if (onCompleted)
                    session.operationQueue_->whenCompleted(operationId, std::move(onCompleted));
                session.resetQueueThrottle();
                return {};
            }
        );
    }

    /**
     * @brief Hands the end of the single upload to @p destination to @p ended, once it can no longer be stopped.
     */
    std::function<void(SharedData::OperationCompleted const&)> forgettingSentUpload(
        Paths::RemotePath const& destination,
        std::function<void(SharedData::OperationCompleted const&)> ended
    ) const
    {
        return [sentUploads = std::weak_ptr{sentUploads_}, destination = destination.value(), ended = std::move(ended)](
                   SharedData::OperationCompleted const& completed
               )
        {
            if (const auto uploads = sentUploads.lock())
                if (const auto found = uploads->find(destination);
                    found != uploads->end() && found->second == completed.operationId)
                    uploads->erase(found);
            if (ended)
                ended(completed);
        };
    }

  private:
    std::weak_ptr<Session> session_;
    /** @brief The single uploads on their way by destination, the tracker may stop them, see cancelUpload(). */
    std::shared_ptr<std::map<std::string, Ids::OperationId>> sentUploads_{
        std::make_shared<std::map<std::string, Ids::OperationId>>()
    };
};

void Session::startFileTracking()
{
    if (!fileTrackingServices_.instances || !fileTrackingServices_.localWatches)
    {
        Log::warn("Session '{}' has no file tracking, it was not started", id_.value());
        return;
    }
    tracker_ = std::make_unique<FileTracking::Tracker>(
        *strand_,
        *fileTrackingServices_.instances,
        *fileTrackingServices_.localWatches,
        std::make_unique<FileTracking::BatchingRemote>(
            *strand_, std::make_unique<FileTrackingRemote>(weak_from_this())
        ),
        std::make_unique<FileTracking::FrontendTrackerView>(*hub_, id_),
        downloadTempFileSuffix_,
        fileTrackingServices_.defaultOptions
    );
}

void Session::stopFileTracking()
{
    if (tracker_)
        tracker_->stopAll();
}

void Session::onConnectionLost()
{
    if (tracker_)
        tracker_->connectionLost();
}

namespace
{
    /**
     * @brief Answers @p reply with {success: true} merged with @p values, or with the error of @p result.
     */
    template <typename T>
    void replyWith(RpcHelper::RpcOnce const& reply, std::expected<T, std::string> const& result, nlohmann::json values)
    {
        if (!result)
            return reply.error(result.error());
        values["success"] = true;
        reply(std::move(values));
    }
}

void Session::registerRpcFileTracker()
{
    const auto name = [this](std::string_view function)
    {
        return fmt::format("FileTracker::{}::{}", id_.value(), function);
    };
    // The session, which holds the tracker for as long as it is used, or nothing after answering that it is gone.
    const auto withTracker = [weak = weak_from_this()](RpcHelper::RpcOnce const& reply) -> std::shared_ptr<Session>
    {
        auto self = weak.lock();
        if (!self || !self->tracker_)
        {
            reply.error("File tracking is not available in this session");
            return nullptr;
        }
        return self;
    };

    on(name("open"))
        .perform(
            [withTracker](RpcHelper::RpcOnce&& reply, std::string const& path, double sizeBytes, bool watch)
            {
                const auto self = withTracker(reply);
                if (!self)
                    return;
                const auto remotePath = RpcHelper::parseRemotePath(path);
                if (!remotePath)
                    return reply.error(remotePath.error());
                self->tracker_->open(
                    *remotePath,
                    static_cast<std::uint64_t>(std::max(sizeBytes, 0.0)),
                    watch,
                    [reply = std::make_shared<RpcHelper::RpcOnce>(std::move(reply))](
                        std::expected<FileTracking::Tracker::OpenedCopy, FileTracking::Tracker::OpenFailure> const&
                            opened
                    )
                    {
                        if (opened)
                            return (*reply)({
                                {"success", true},
                                {"instanceId", opened->instanceId.value()},
                                {"localPath", opened->localPath.toUtf8()},
                            });
                        if (opened.error())
                            return reply->error(*opened.error());
                        (*reply)({{"success", false}});
                    }
                );
            }
        );

    on(name("checkUploadAndWatch"))
        .perform(
            [withTracker](RpcHelper::RpcOnce&& reply, std::string const& item, std::string const& directory)
            {
                const auto self = withTracker(reply);
                if (!self)
                    return;
                const auto localItem = RpcHelper::parseLocalPath(item);
                const auto remoteDirectory = RpcHelper::parseRemotePath(directory);
                if (!localItem || !remoteDirectory)
                    return reply.error(!localItem ? localItem.error() : remoteDirectory.error());
                const auto target = self->tracker_->checkUploadAndWatch(*localItem, *remoteDirectory);
                const auto values = target
                    ? nlohmann::json{{"item", target->item.toUtf8()}, {"destination", target->destination.value()}}
                    : nlohmann::json::object();
                replyWith(reply, target, values);
            }
        );

    on(name("uploadAndWatch"))
        .perform(
            [withTracker](RpcHelper::RpcOnce&& reply, std::string const& item, std::string const& destination)
            {
                const auto self = withTracker(reply);
                if (!self)
                    return;
                const auto localItem = RpcHelper::parseLocalPath(item);
                const auto remoteDestination = RpcHelper::parseRemotePath(destination);
                if (!localItem || !remoteDestination)
                    return reply.error(!localItem ? localItem.error() : remoteDestination.error());
                replyWith(
                    reply, self->tracker_->uploadAndWatch(*localItem, *remoteDestination), nlohmann::json::object()
                );
            }
        );

    on(name("stop"))
        .perform(
            [withTracker](RpcHelper::RpcOnce&& reply, std::string const& watchId)
            {
                if (const auto self = withTracker(reply))
                {
                    self->tracker_->stop(Ids::makeWatchId(watchId));
                    reply({{"success", true}});
                }
            }
        );

    on(name("setOption"))
        .perform(
            [withTracker](
                RpcHelper::RpcOnce&& reply, std::string const& watchId, SharedData::WatchOption option, bool value
            )
            {
                if (const auto self = withTracker(reply))
                {
                    self->tracker_->setOption(Ids::makeWatchId(watchId), option, value);
                    reply({{"success", true}});
                }
            }
        );

    on(name("discard"))
        .perform(
            [withTracker](RpcHelper::RpcOnce&& reply, std::string const& instanceId, std::string const& path)
            {
                const auto self = withTracker(reply);
                if (!self)
                    return;
                const auto localPath = RpcHelper::parseLocalPath(path);
                if (!localPath)
                    return reply.error(localPath.error());
                const auto discarded = self->tracker_->discard(Ids::makeInstanceId(instanceId), *localPath);
                if (!discarded)
                    Log::error("FileTracking: discarding '{}' failed: {}", localPath->toUtf8(), discarded.error());
                replyWith(
                    reply,
                    discarded,
                    {{"instanceDestroyed", discarded == FileTracking::DiscardOutcome::InstanceDestroyed}}
                );
            }
        );

    on(name("answer"))
        .perform(
            [withTracker](
                RpcHelper::RpcOnce&& reply, std::string const& questionId, SharedData::ServerChangedAnswer answer
            )
            {
                if (const auto self = withTracker(reply))
                {
                    self->tracker_->answer(Ids::makeQuestionId(questionId), answer);
                    reply({{"success", true}});
                }
            }
        );

    // Registers a function that calls @p call on the tracker and answers {success: true}.
    const auto onCall = [this, &name, &withTracker](std::string_view function, auto call)
    {
        on(name(function))
            .perform(
                [withTracker, call](RpcHelper::RpcOnce&& reply)
                {
                    if (const auto self = withTracker(reply))
                    {
                        std::invoke(call, *self->tracker_);
                        reply({{"success", true}});
                    }
                }
            );
    };
    // Like onCall, @p call gets the id the frontend sent, made with @p makeId.
    const auto onIdCall = [this, &name, &withTracker](std::string_view function, auto makeId, auto call)
    {
        on(name(function))
            .perform(
                [withTracker, makeId, call](RpcHelper::RpcOnce&& reply, std::string const& id)
                {
                    if (const auto self = withTracker(reply))
                    {
                        std::invoke(call, *self->tracker_, makeId(id));
                        reply({{"success", true}});
                    }
                }
            );
    };
    // Like onIdCall, @p call gets the resolution the frontend sent too.
    const auto onAnswerCall = [this, &name, &withTracker](std::string_view function, auto makeId, auto call)
    {
        on(name(function))
            .perform(
                [withTracker,
                    makeId,
                    call](RpcHelper::RpcOnce&& reply, std::string const& id, SharedData::ConflictResolution resolution)
                {
                    if (const auto self = withTracker(reply))
                    {
                        std::invoke(call, *self->tracker_, makeId(id), resolution);
                        reply({{"success", true}});
                    }
                }
            );
    };

    onAnswerCall("answerConflict", &Ids::makeConflictId, &FileTracking::Tracker::answerConflict);
    onAnswerCall("answerAllConflicts", &Ids::makeWatchId, &FileTracking::Tracker::answerAllConflicts);
    onIdCall("retryFailure", &Ids::makeFailureId, &FileTracking::Tracker::retryFailure);
    onIdCall("dismissFailure", &Ids::makeFailureId, &FileTracking::Tracker::dismissFailure);
    onIdCall("retryAllFailures", &Ids::makeWatchId, &FileTracking::Tracker::retryAllFailures);
    onIdCall("dismissAllFailures", &Ids::makeWatchId, &FileTracking::Tracker::dismissAllFailures);
    onIdCall("removeStopped", &Ids::makeWatchId, &FileTracking::Tracker::removeStopped);
    onCall("removeAllStopped", &FileTracking::Tracker::removeAllStopped);
    onCall("listOrphanedCopies", &FileTracking::Tracker::listOrphanedCopies);
    onCall("removeAllOrphanedCopies", &FileTracking::Tracker::removeAllOrphanedCopies);

    on(name("removeOrphanedCopy"))
        .perform(
            [withTracker](RpcHelper::RpcOnce&& reply, std::string const& instanceId)
            {
                if (const auto self = withTracker(reply))
                    replyWith(
                        reply,
                        self->tracker_->removeOrphanedCopy(Ids::makeInstanceId(instanceId)),
                        nlohmann::json::object()
                    );
            }
        );
}
