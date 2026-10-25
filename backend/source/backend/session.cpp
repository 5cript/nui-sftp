#include <backend/session.hpp>
#include <utility/error_message.hpp>
#include <backend/rpc_helper.hpp>
#include <backend/rpc_path_parameters.hpp>
#include <utility/localized_message.hpp>
#include <backend/existence_check.hpp>

#include <paths/paths.hpp>

#include <roar/utility/base64.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/file_operations/bulk_delete_request.hpp>
#include <shared_data/sync/sync_execution.hpp>
#include <tar_archive/compression.hpp>

#include <nui/utility/scope_exit.hpp>

#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace std::chrono_literals;

namespace
{
    /**
     * @brief The reply to a request that queued one operation, or why it could not be queued.
     */
    std::expected<nlohmann::json, std::string> queuedReply(std::expected<void, Operation::Error> const& queued)
    {
        return queued
            .transform(
                []()
                {
                    return nlohmann::json{{"success", true}};
                }
            )
            .transform_error(
                [](Operation::Error const& error)
                {
                    return error.toString();
                }
            );
    }

    /**
     * @brief The reply to a request that queued @p enqueued entries.
     */
    nlohmann::json enqueuedReply(std::size_t enqueued)
    {
        return {{"success", true}, {"enqueued", enqueued}};
    }

    /**
     * @brief Answers a delete request with the localized reason why nothing was deleted.
     */
    void replyRefusal(RpcHelper::RpcOnce const& reply, RemoteDeleteRefusal const& refusal)
    {
        Log::error("Refusing to delete: {}", refusal.toString());
        reply.error(refusal.toLocalizedMessage());
    }
}

Session::Session(
    Ids::SessionId id,
    std::unique_ptr<SecureShell::Session> session,
    boost::asio::any_io_executor executor,
    std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    Persistence::SftpOptions const& sftpOptions,
    BulkResumeRegistry* bulkResumeRegistry,
    std::shared_ptr<LocalOperationQueue> localOperationQueue,
    FileTracking::TrackerServices fileTracking
)
    : RpcHelper::StrandRpc{executor, std::move(strand), wnd, hub}
    , id_{std::move(id)}
    , session_{std::move(session)}
    , operationQueue_{std::make_shared<OperationQueue>(
          executor_,
          strand_,
          wnd,
          hub,
          sftpOptions,
          id_,
          sftpOptions.concurrency.value_or(Persistence::defaultSftpConcurrency)
      )}
    , bulkResumeRegistry_{bulkResumeRegistry}
    , remoteTempFileSuffix_{Persistence::effectiveTempFileSuffix(
          sftpOptions.uploadOptions ? sftpOptions.uploadOptions->tempFileSuffix : std::nullopt
      )}
    , localOperationQueue_{std::move(localOperationQueue)}
    , fileTrackingServices_{std::move(fileTracking)}
    , downloadTempFileSuffix_{Persistence::effectiveTempFileSuffix(
          sftpOptions.downloadOptions ? sftpOptions.downloadOptions->tempFileSuffix : std::nullopt
      )}
{}

Session::~Session()
{
    stop();
}

void Session::start()
{
    within_strand_do(
        [weak = weak_from_this()]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            if (!self->session_)
            {
                Log::error("Session is not valid, cannot start");
                return;
            }

            self->session_->start();
            self->registerRpcCreateChannel();
            self->registerRpcStartChannelRead();
            self->registerRpcChannelClose();
            self->registerRpcChannelWrite();
            self->registerRpcChannelPtyResize();
            self->registerRpcSftpListDirectory();
            self->registerRpcSftpHomeDirectory();
            self->registerRpcSftpUserDirectories();
            self->registerRpcSftpCreateDirectory();
            self->registerRpcSftpCreateFile();
            self->registerRpcSftpAddDownloadOperation();
            self->registerRpcSftpAddUploadOperation();
            self->registerRpcSftpAddArchiveDownloadOperation();
            self->registerRpcSftpAddArchiveUploadOperation();
            self->registerRpcSftpAddRenameOperation();
            self->registerRpcSftpAddBulkDownloadOperation();
            self->registerRpcSftpAddBulkUploadOperation();
            self->registerRpcSftpAddBulkDeleteOperation();
            self->registerRpcAddLocalDeleteOperation();
            self->registerRpcSftpExistsBatch();
            self->registerRpcSftpOpenSyncSession();
            self->registerRpcSftpRecomputeSyncDiff();
            self->registerRpcSftpLoadSyncDiffChildren();
            self->registerRpcSftpExecuteSyncSelection();
            self->registerRpcSftpCancelSyncDiff();
            self->registerRpcSftpCloseSyncSession();
            self->registerOperationQueuePauseUnpause();
            self->registerRpcSftpDeleteFiles();
            self->registerRpcSftpRename();
            self->registerRpcSftpStat();
            self->registerRpcSftpPreDeleteChecks();
            self->registerRpcQueuedRemoves();
            self->operationQueue_->registerRpc();
            self->startFileTracking();
            self->registerRpcFileTracker();

            Log::info("Session '{}' connected", self->id_.value());

            self->running_ = true;
            self->operationQueue_->setWake(
                [weak = self->weak_from_this()]()
                {
                    if (auto session = weak.lock(); session)
                        session->wakeOperationQueue();
                }
            );
            self->scheduleOperationQueueWork();
        }
    );
}

void Session::stop()
{
    running_ = false;
    timer_.cancel();

    std::promise<void> waitForPostedWork;
    within_strand_do(
        [weak = weak_from_this(), &waitForPostedWork]() mutable
        {
            const auto scopeExit = Nui::ScopeExit(
                [&waitForPostedWork]() noexcept
                {
                    waitForPostedWork.set_value();
                }
            );

            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            Log::info("Stopping session '{}'", self->id_.value());

            // Before the queue goes, whose cancellations would otherwise reach the tracker. The copies of the session
            // count as orphaned from now on.
            self->stopFileTracking();
            self->tracker_.reset();

            // Backup any in-flight or queued bulk operations before
            // teardown so a seamless reconnect can adopt them.  Always
            // happens — the frontend's adopt/discard signals (and the
            // registry TTL) handle cleanup if no resume occurs.
            if (self->bulkResumeRegistry_)
                self->operationQueue_->dumpBulkResumes(*self->bulkResumeRegistry_);
            self->localOperationQueue_->release(self->id_);
            self->operationQueue_->dropAll();
            self->operationQueue_.reset();

            // Close all channels
            for (auto& [channelId, weakChannel] : self->channels_)
            {
                if (auto channel = weakChannel.lock(); channel)
                {
                    Log::info("Closing channel '{}' for session '{}'", channelId.value(), self->id_.value());
                    channel->close();
                }
            }
            self->channels_.clear();

            // Close all sftp channels
            for (auto& [channelId, weakChannel] : self->sftpChannels_)
            {
                if (auto channel = weakChannel.lock(); channel)
                {
                    Log::info("Closing sftp channel '{}' for session '{}'", channelId.value(), self->id_.value());
                    channel->close();
                }
            }
            self->sftpChannels_.clear();

            // Close session
            if (self->session_)
            {
                Log::info("Closing ssh session '{}'", self->id_.value());
                self->session_.reset();
            }
        }
    );

    waitForPostedWork.get_future().get();
}

void Session::adoptBulkResumes(std::vector<Ids::OperationId> const& operationIds)
{
    if (operationIds.empty())
        return;

    within_strand_do(
        [weak = weak_from_this(), operationIds]()
        {
            auto self = weak.lock();
            if (!self || !self->operationQueue_)
                return;
            if (!self->bulkResumeRegistry_)
            {
                Log::warn("Session::adoptBulkResumes called without a registry — ignoring");
                return;
            }
            // Resume needs an SFTP channel.  All currently-supported bulk
            // kinds re-enter the same Session::sftp::addBulk* code paths,
            // which expect a channel id.  We adopt against the most-recently
            // opened sftp channel by convention; if none exists, the resume
            // is dropped back into the registry for a later attempt.
            const auto sftp = self->anySftpChannel();
            if (!sftp)
            {
                Log::warn(
                    "Session::adoptBulkResumes: no sftp channel open yet — deferring {} resume(s)", operationIds.size()
                );
                return;
            }

            for (auto const& opId : operationIds)
            {
                auto entry = self->bulkResumeRegistry_->take(opId);
                if (!entry)
                {
                    Log::info(
                        "Session::adoptBulkResumes: no backup for operation '{}' (already evicted?)", opId.value()
                    );
                    continue;
                }
                self->operationQueue_->adoptBulkResume(*sftp, opId, std::move(*entry));
            }
        }
    );
}

std::shared_ptr<SecureShell::SftpSession> Session::anySftpChannel() const
{
    for (auto const& [channelId, weakChannel] : sftpChannels_)
    {
        if (auto locked = weakChannel.lock(); locked)
            return locked;
    }
    return nullptr;
}

void Session::doOperationQueueWork()
{
    operationQueueWorkScheduled_ = false;
    if (!running_ || !operationQueue_)
        return;

    // Posted, not looped: requests on the strand get their turn between steps. An operation that waits for its
    // transfer, a paused or an empty queue wake the queue once there is work again.
    if ((!operationQueue_->paused() || operationQueue_->hasPriorityWork()) && operationQueue_->work())
        scheduleOperationQueueWork();
}

void Session::wakeOperationQueue()
{
    within_strand_do(
        [weak = weak_from_this()]()
        {
            if (auto self = weak.lock(); self)
                self->scheduleOperationQueueWork();
        }
    );
}

void Session::scheduleOperationQueueWork()
{
    if (operationQueueWorkScheduled_)
        return;
    operationQueueWorkScheduled_ = true;
    boost::asio::post(
        *strand_,
        [weak = weak_from_this()]()
        {
            if (auto self = weak.lock(); self)
                self->doOperationQueueWork();
        }
    );
}

void Session::removeChannel(Ids::ChannelId channelId)
{
    within_strand_do(
        [weak = weak_from_this(), channelId = std::move(channelId)]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            auto iter = self->channels_.find(channelId);
            if (iter != self->channels_.end())
            {
                if (auto locked = iter->second.lock(); locked)
                    locked->close();

                self->channels_.erase(iter);
            }
            else
            {
                Log::warn("Cannot remove channel, no channel found with id: {}", channelId.value());
            }
        }
    );
}

void Session::removeSftpChannel(Ids::ChannelId channelId)
{
    within_strand_do(
        [weak = weak_from_this(), channelId = std::move(channelId)]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            auto iter = self->sftpChannels_.find(channelId);
            if (iter != self->sftpChannels_.end())
            {
                if (auto locked = iter->second.lock(); locked)
                    locked->close();

                self->sftpChannels_.erase(iter);
            }
            else
            {
                Log::warn("Cannot remove sftp channel, no channel found with id: {}", channelId.value());
            }
        }
    );
}

void Session::registerRpcCreateChannel()
{
    on(fmt::format("Session::{}::Channel::create", id_.value()))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                RpcHelper::ParameterVerifyView verify{
                    reply, fmt::format("Session::{}::Channel::create", self->id_.value()), parameters
                };

                if (!verify.hasValueDeep("engineOptions"))
                    return;
                if (!verify.hasValueDeep("termios"))
                    return;

                const bool fileMode = parameters.contains("fileMode") && parameters["fileMode"].is_boolean() &&
                    parameters["fileMode"].get<bool>();

                if (!fileMode)
                {
                    Log::info("Creating pty channel for session '{}'", self->id_.value());
                    Log::info("Session options: {}", parameters["engineOptions"].dump());
                    Log::info("Termios: {}", parameters["termios"].dump());

                    const auto sessionOptions = parameters["engineOptions"].get<Persistence::SshSessionOptions>();
                    const auto termios = parameters["termios"].get<Persistence::Termios>();

                    const auto weakChannel = self->session_
                                                 ->createPtyChannel(
                                                     {.environment = sessionOptions.sshOptions->environment,
                                                         .localeEnv = sessionOptions.sshOptions->localeEnv,
                                                         .termios = termios}
                                                 )
                                                 .get();
                    if (!weakChannel.has_value())
                    {
                        Log::error("Failed to create pty channel: {}", weakChannel.error());
                        return reply.error(fmt::format("Failed to create pty channel: {}", weakChannel.error()));
                    }

                    const auto channelId = Ids::generateChannelId();
                    self->channels_.emplace(channelId, std::move(weakChannel).value());

                    Log::info(
                        "Created pty channel with id '{}', channel total is now '{}'.",
                        channelId.value(),
                        self->channels_.size()
                    );

                    return reply({{"id", channelId.value()}});
                }
                else
                {
                    Log::info("Creating sftp channel for session '{}'", self->id_.value());

                    const auto weakChannel = self->session_->createSftpSession().get();
                    if (!weakChannel.has_value())
                    {
                        Log::error("Failed to create sftp channel: {}", weakChannel.error().toString());
                        return reply.error("Failed to create sftp channel: " + weakChannel.error().toString());
                    }

                    const auto channelId = Ids::generateChannelId();
                    self->sftpChannels_.emplace(channelId, std::move(weakChannel).value());

                    Log::info(
                        "Created sftp channel with id '{}', sftp channel total is now '{}'.",
                        channelId.value(),
                        self->sftpChannels_.size()
                    );

                    return reply({{"id", channelId.value()}});
                }
            }
        );
}

void Session::registerRpcStartChannelRead()
{
    on(fmt::format("Session::{}::Channel::startReading", id_.value()))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, std::string const& channelIdString)
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                const auto channelId = Ids::makeChannelId(channelIdString);
                if (self->channels_.find(channelId) == self->channels_.end())
                {
                    Log::error("No channel found with id: {}", channelId.value());
                    return reply.error("No channel found with id");
                }

                auto locked = self->channels_[channelId].lock();
                if (!locked)
                {
                    Log::error("Failed to lock channel with id: {}", channelId.value());
                    self->removeChannel(channelId);
                    return reply.error("Failed to lock channel");
                }

                // Do not use this in these functions, because they are called from a different thread,
                // unless within_strand_do is used!
                locked->startReading(
                    // Stdout
                    [sessionId = self->id_,
                        channelId,
                        stdOut =
                            RpcHelper::RpcInCorrectThread{
                                *self->wnd_,
                                *self->hub_,
                                fmt::format("sshTerminalStdout_{}", channelId.value()),
                            }](std::string const& msg)
                    {
                        stdOut(
                            nlohmann::json{
                                {"sessionId", sessionId.value()},
                                {"channelId", channelId.value()},
                                {"data", Roar::base64Encode(msg)},
                            }
                        );
                    },
                    // Stderr
                    [sessionId = self->id_,
                        channelId,
                        stdErr =
                            RpcHelper::RpcInCorrectThread{
                                *self->wnd_,
                                *self->hub_,
                                fmt::format("sshTerminalStderr_{}", channelId.value()),
                            }](std::string const& data)
                    {
                        stdErr(
                            nlohmann::json{
                                {"sessionId", sessionId.value()},
                                {"channelId", channelId.value()},
                                {"data", Roar::base64Encode(data)},
                            }
                        );
                    },
                    // On channel exit:
                    [weak = self->weak_from_this(), sessionId = self->id_, channelId]
                    {
                        Log::info("Channel for session '{}' lost with id: {}.", sessionId.value(), channelId.value());
                        auto self = weak.lock();
                        if (!self)
                            return;
                        self->hub_->call(fmt::format("sshTerminalOnExit_{}", channelId.value()));
                        self->removeChannel(channelId);
                    }
                );

                return reply({{"success", true}});
            }
        );
}

void Session::registerRpcChannelClose()
{
    on(fmt::format("Session::{}::Channel::close", id_.value()))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, std::string const& channelIdString)
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->removeChannel(Ids::makeChannelId(channelIdString));
                return reply({{"success", true}});
            }
        );
}

void Session::registerRpcChannelWrite()
{
    on(fmt::format("Session::{}::Channel::write", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string&& data
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [data = std::move(data)](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        channel->write(Roar::base64Decode(data));
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcChannelPtyResize()
{
    on(fmt::format("Session::{}::Channel::ptyResize", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, int cols, int rows
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [cols, rows](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        channel->resizePty(cols, rows);
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpListDirectory()
{
    on(fmt::format("Session::{}::sftp::listDirectory", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& path
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [path](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        const auto remotePath = RpcHelper::parseRemotePath(path);
                        if (!remotePath)
                            return reply.error(remotePath.error());
                        auto fut = channel->listDirectory(*remotePath);
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Timed out");

                        const auto result = fut.get();
                        if (!result.has_value())
                        {
                            const auto& error = result.error();
                            if (!error.message.empty())
                                return reply.error(error.message);
                            if (error.sftpError != SSH_FX_OK)
                                return reply.error(SecureShell::SftpError::sftpErrorToComprehensible(error.sftpError));
                            return reply.error("");
                        }

                        Log::info("Listed directory '{}', got {} entries", path, result->size());
                        reply({{"entries", *result}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpHomeDirectory()
{
    on(fmt::format("Session::{}::sftp::homeDirectory", id_.value()))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, std::string const& channelIdString)
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto fut = channel->homeDirectory();
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Failed to get home directory: timeout");

                        const auto result = fut.get();
                        if (!result.has_value())
                            return reply.error("Failed to get home directory: " + result.error().toString());

                        const auto& home = result->value();
                        Log::info("Remote home directory is '{}'", home);
                        reply({{"path", home}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpUserDirectories()
{
    on(fmt::format("Session::{}::sftp::userDirectories", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& home
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [home](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        const auto homePath = RpcHelper::parseRemotePath(home);
                        if (!homePath)
                            return reply.error(homePath.error());
                        auto fut = channel->userDirectories(*homePath);
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Failed to read user directories: timeout");

                        const auto result = fut.get();
                        if (!result.has_value())
                            return reply.error("Failed to read user directories: " + result.error().toString());

                        auto directories = nlohmann::json::array();
                        for (auto const& directory : *result)
                        {
                            directories.push_back(
                                {{"name", directory.name}, {"path", directory.path}}
                            );
                        }
                        Log::info("Read {} user directories in '{}'", directories.size(), home);
                        reply({{"directories", std::move(directories)}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcQueuedRemoves()
{
    on(fmt::format("Session::{}::sftp::queuedRemove", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::vector<std::string> const& pathStrings,
                bool recursive,
                bool insertRefresh,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto parsed = RpcHelper::parseRemotePaths(pathStrings);
                if (!parsed)
                    return reply.error(parsed.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [paths = *std::move(parsed), recursive, insertRefresh, mode](
                        RpcHelper::RpcOnce&& reply,
                        Session& self,
                        std::shared_ptr<SecureShell::SftpSession> const& channel
                    ) mutable
                    {
                        if (!self.operationQueue_)
                            return reply.error("Session no longer exists");

                        auto guard = self.operationQueue_->remoteDeleteGuard(*channel);
                        if (const auto refusal = refuseAnyProtected(guard, paths); refusal)
                            return replyRefusal(reply, *refusal);

                        std::vector<std::string> operationIds;
                        for (auto const& path : paths)
                        {
                            const auto newOperationId = Ids::generateOperationId();
                            const auto result = self.operationQueue_->addDeleteOperation(
                                guard,
                                *channel,
                                newOperationId,
                                path,
                                recursive,
                                insertRefresh,
                                static_cast<SharedData::OperationMode>(mode)
                            );
                            if (!result)
                            {
                                Log::error("Could not queue the delete of '{}': {}", path.value(), result.error().toString());
                                continue;
                            }
                            operationIds.push_back(newOperationId.value());
                        }

                        reply({{"operationIds", operationIds}, {"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpCreateDirectory()
{
    on(fmt::format("Session::{}::sftp::createDirectory", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& path
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [path](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        const auto remotePath = RpcHelper::parseRemotePath(path);
                        if (!remotePath)
                            return reply.error(remotePath.error());
                        auto fut = channel->createDirectory(*remotePath);
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error(fmt::format("Failed to create directory '{}': timeout", path));

                        const auto result = fut.get();
                        if (!result.has_value())
                            return reply.error(
                                fmt::format("Failed to create directory '{}': {}", path, result.error().toString())
                            );

                        Log::info("Created directory '{}'", path);
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpCreateFile()
{
    on(fmt::format("Session::{}::sftp::createFile", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& path
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [path](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        const auto remotePath = RpcHelper::parseRemotePath(path);
                        if (!remotePath)
                            return reply.error(remotePath.error());
                        auto fut = channel->createFile(*remotePath);
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Failed to create file: timeout");

                        const auto result = fut.get();
                        if (!result.has_value())
                            return reply.error("Failed to create file: " + result.error().message);

                        Log::info("Created file '{}'", path);
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpAddDownloadOperation()
{
    on(fmt::format("Session::{}::sftp::addDownload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& newOperationIdString,
                std::string const& remotePath,
                std::string const& localPath,
                bool allowOverwrite,
                bool bigFile,
                bool insertRefresh,
                bool createMissingDirectories,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto remote = RpcHelper::parseRemotePath(remotePath);
                if (!remote)
                    return reply.error(remote.error());
                auto local = RpcHelper::parseLocalPath(localPath);
                if (!local)
                    return reply.error(local.error());

                auto description = fmt::format(
                    "the download '{}' of '{}' to '{}'", newOperationIdString, remote->value(), local->toUtf8()
                );
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationId = Ids::makeOperationId(newOperationIdString),
                        local = *std::move(local),
                        remote = *std::move(remote),
                        allowOverwrite,
                        bigFile,
                        insertRefresh,
                        createMissingDirectories,
                        mode](OperationQueue& queue, SecureShell::SftpSession& channel)
                    {
                        return queuedReply(queue.addDownloadOperation(
                            channel,
                            operationId,
                            local,
                            remote,
                            allowOverwrite,
                            bigFile,
                            insertRefresh,
                            createMissingDirectories,
                            static_cast<SharedData::OperationMode>(mode)
                        ));
                    }
                );
            }
        );
}

void Session::registerRpcSftpAddArchiveDownloadOperation()
{
    // One RPC to pack N remote files into a single local tar[.ext] archive.
    on(fmt::format("Session::{}::sftp::addArchiveDownload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& newOperationIdString,
                std::string const& rootsJson,
                std::string const& localArchivePath,
                int compressionCodec,
                int compressionLevel,
                bool xzExtreme,
                bool mayOverwrite,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto roots = RpcHelper::parseRequest<std::vector<SharedData::ArchiveDownloadRoot>>(
                    nlohmann::json::parse(rootsJson, nullptr, false), "archive roots"
                );
                if (!roots)
                    return reply.error(roots.error());
                auto archivePath = RpcHelper::parseLocalPath(localArchivePath);
                if (!archivePath)
                    return reply.error(archivePath.error());

                const auto compression = static_cast<TarArchive::Compression>(compressionCodec);
                const auto compressionOptions =
                    TarArchive::compressionOptionsForLevel(compression, compressionLevel, xzExtreme);
                auto description = fmt::format(
                    "the archive download '{}' of {} roots to '{}'",
                    newOperationIdString,
                    roots->size(),
                    archivePath->toUtf8()
                );
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationId = Ids::makeOperationId(newOperationIdString),
                        roots = *std::move(roots),
                        archivePath = *std::move(archivePath),
                        compression,
                        compressionOptions,
                        mayOverwrite,
                        mode](OperationQueue& queue, SecureShell::SftpSession& channel) mutable
                    {
                        return queuedReply(queue.addArchiveDownloadOperation(
                            channel,
                            operationId,
                            std::move(roots),
                            archivePath,
                            compression,
                            compressionOptions,
                            mayOverwrite,
                            static_cast<SharedData::OperationMode>(mode)
                        ));
                    }
                );
            }
        );
}

void Session::registerRpcSftpAddArchiveUploadOperation()
{
    on(fmt::format("Session::{}::sftp::addArchiveUpload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& newOperationIdString,
                std::vector<std::string> const& localPathStrings,
                std::string const& remoteArchivePath,
                int compressionCodec,
                int compressionLevel,
                bool xzExtreme,
                bool mayOverwrite,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto localPaths = RpcHelper::parseLocalPaths(localPathStrings);
                if (!localPaths)
                    return reply.error(localPaths.error());
                auto archivePath = RpcHelper::parseRemotePath(remoteArchivePath);
                if (!archivePath)
                    return reply.error(archivePath.error());

                const auto compression = static_cast<TarArchive::Compression>(compressionCodec);
                const auto compressionOptions =
                    TarArchive::compressionOptionsForLevel(compression, compressionLevel, xzExtreme);
                auto description = fmt::format(
                    "the archive upload '{}' of {} paths to '{}'",
                    newOperationIdString,
                    localPaths->size(),
                    archivePath->value()
                );
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationId = Ids::makeOperationId(newOperationIdString),
                        localPaths = *std::move(localPaths),
                        archivePath = *std::move(archivePath),
                        compression,
                        compressionOptions,
                        mayOverwrite,
                        mode](OperationQueue& queue, SecureShell::SftpSession& channel) mutable
                    {
                        return queuedReply(queue.addArchiveUploadOperation(
                            channel,
                            operationId,
                            std::move(localPaths),
                            archivePath,
                            compression,
                            compressionOptions,
                            mayOverwrite,
                            static_cast<SharedData::OperationMode>(mode)
                        ));
                    }
                );
            }
        );
}

void Session::registerRpcSftpAddBulkDownloadOperation()
{
    // One RPC and one strand dispatch for a whole batch. Files are trusted, there is no lstat per entry.
    on(fmt::format("Session::{}::sftp::addBulkDownload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::vector<std::string>&& operationIdStrings,
                nlohmann::json const& requestJson
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto request = RpcHelper::parseRequest<SharedData::BulkDownloadRequest>(requestJson, "bulk download");
                if (!request)
                    return reply.error(request.error());
                // An id per entry and the id of the card of the files at the end.
                if (operationIdStrings.size() != request->entries.size() + 1)
                    return reply.error("addBulkDownload: operationIds and entries length mismatch");

                auto description = fmt::format("the bulk download of {} entries", request->entries.size());
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationIds = std::move(operationIdStrings), request = *std::move(request)](
                        OperationQueue& queue, SecureShell::SftpSession& channel
                    ) mutable -> std::expected<nlohmann::json, std::string>
                    {
                        const auto bulkCardId = Ids::makeOperationId(operationIds.back());
                        return enqueuedReply(queue.addBulkDownloadOperation(
                            channel,
                            std::move(request),
                            [&operationIds](std::size_t index)
                            {
                                return Ids::makeOperationId(operationIds[index]);
                            },
                            bulkCardId
                        ));
                    }
                );
            }
        );
}

void Session::registerRpcSftpAddBulkUploadOperation()
{
    // Same contract as addBulkDownload.
    on(fmt::format("Session::{}::sftp::addBulkUpload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::vector<std::string>&& operationIdStrings,
                nlohmann::json const& requestJson
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto request = RpcHelper::parseRequest<SharedData::BulkUploadRequest>(requestJson, "bulk upload");
                if (!request)
                    return reply.error(request.error());
                if (operationIdStrings.size() != request->entries.size() + 1)
                    return reply.error("addBulkUpload: operationIds and entries length mismatch");

                auto description = fmt::format("the bulk upload of {} entries", request->entries.size());
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationIds = std::move(operationIdStrings), request = *std::move(request)](
                        OperationQueue& queue, SecureShell::SftpSession& channel
                    ) mutable -> std::expected<nlohmann::json, std::string>
                    {
                        const auto bulkCardId = Ids::makeOperationId(operationIds.back());
                        return enqueuedReply(queue.addBulkUploadOperation(
                            channel,
                            std::move(request),
                            [&operationIds](std::size_t index)
                            {
                                return Ids::makeOperationId(operationIds[index]);
                            },
                            bulkCardId
                        ));
                    }
                );
            }
        );
}

void Session::registerRpcSftpAddUploadOperation()
{
    on(fmt::format("Session::{}::sftp::addUpload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& newOperationIdString,
                std::string const& localPath,
                std::string const& remotePath,
                bool allowOverwrite,
                bool bigFile,
                bool insertRefresh,
                bool createMissingDirectories,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto local = RpcHelper::parseLocalPath(localPath);
                if (!local)
                    return reply.error(local.error());
                auto remote = RpcHelper::parseRemotePath(remotePath);
                if (!remote)
                    return reply.error(remote.error());

                auto description = fmt::format(
                    "the upload '{}' of '{}' to '{}'", newOperationIdString, local->toUtf8(), remote->value()
                );
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationId = Ids::makeOperationId(newOperationIdString),
                        local = *std::move(local),
                        remote = *std::move(remote),
                        allowOverwrite,
                        bigFile,
                        insertRefresh,
                        createMissingDirectories,
                        mode](OperationQueue& queue, SecureShell::SftpSession& channel)
                    {
                        return queuedReply(queue.addUploadOperation(
                            channel,
                            operationId,
                            local,
                            remote,
                            allowOverwrite,
                            bigFile,
                            insertRefresh,
                            createMissingDirectories,
                            static_cast<SharedData::OperationMode>(mode)
                        ));
                    }
                );
            }
        );
}

void Session::registerRpcSftpAddRenameOperation()
{
    on(fmt::format("Session::{}::sftp::addRename", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& newOperationIdString,
                std::string const& sourcePath,
                std::string const& destinationPath,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto source = RpcHelper::parseRemotePath(sourcePath);
                if (!source)
                    return reply.error(source.error());
                auto destination = RpcHelper::parseRemotePath(destinationPath);
                if (!destination)
                    return reply.error(destination.error());

                auto description = fmt::format(
                    "the rename '{}' of '{}' to '{}'", newOperationIdString, source->value(), destination->value()
                );
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationId = Ids::makeOperationId(newOperationIdString),
                        source = *std::move(source),
                        destination = *std::move(destination),
                        mode](OperationQueue& queue, SecureShell::SftpSession& channel)
                    {
                        return queuedReply(queue.addRenameOperation(
                            channel, operationId, source, destination, static_cast<SharedData::OperationMode>(mode)
                        ));
                    }
                );
            }
        );
}

void Session::registerOperationQueuePauseUnpause()
{
    on(fmt::format("OperationQueue::{}::pauseUnpause", id_.value()))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, bool pause)
            {
                auto self = weak.lock();
                if (!self)
                {
                    Log::error("Session no longer exists, cannot pause/unpause operation queue");
                    return reply(SharedData::error("Session no longer exists"));
                }

                if (!self->operationQueue_)
                {
                    Log::error("No operation queue available to pause/unpause");
                    return reply(SharedData::error("No operation queue available"));
                }

                self->operationQueue_->paused(pause);
                self->localOperationQueue_->hold(self->id_, pause);
                self->wakeOperationQueue();
                return reply(SharedData::success());
            }
        );
}
void Session::registerRpcSftpOpenSyncSession()
{
    on(fmt::format("Session::{}::sftp::openSyncSession", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& syncSessionIdString,
                std::string const& remoteScanIdString,
                std::string const& localScanIdString,
                std::string const& remotePath,
                std::string const& localPath,
                bool respectIgnoreFiles,
                bool recursive,
                bool ignoreHidden
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                // The roots every synchronized path is resolved against, refused here when they are not absolute.
                auto remoteRoot = RpcHelper::parseRemotePath(remotePath);
                if (!remoteRoot)
                    return reply.error(remoteRoot.error());
                auto localRoot = RpcHelper::parseLocalPath(localPath);
                if (!localRoot)
                    return reply.error(localRoot.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [syncSessionIdString,
                        remoteScanIdString,
                        localScanIdString,
                        remotePath = *std::move(remoteRoot),
                        localPath = *std::move(localRoot),
                        respectIgnoreFiles,
                        recursive,
                        ignoreHidden](
                        RpcHelper::RpcOnce&& reply,
                        Session& self,
                        std::shared_ptr<SecureShell::SftpSession> const& channel
                    )
                    {
                        if (!self.operationQueue_)
                            return reply.error("Session no longer exists");

                        self.operationQueue_->addSyncScanOperation(
                            *channel,
                            Ids::makeSyncSessionId(syncSessionIdString),
                            Ids::makeOperationId(remoteScanIdString),
                            Ids::makeOperationId(localScanIdString),
                            remotePath,
                            localPath,
                            respectIgnoreFiles,
                            recursive,
                            ignoreHidden
                        );

                        self.wakeOperationQueue();
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpRecomputeSyncDiff()
{
    on(fmt::format("Session::{}::sftp::recomputeSyncDiff", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& syncSessionIdString,
                SharedData::Sync::DiffOptions const& options,
                SharedData::Sync::LoadedParents const& loadedParents
            )
            {
                auto self = weak.lock();
                if (!self || !self->operationQueue_)
                    return reply({{"error", "Session no longer exists"}});

                const auto syncSessionId = Ids::makeSyncSessionId(syncSessionIdString);
                auto session = self->operationQueue_->syncSession(syncSessionId);
                if (!session)
                    return reply({{"error", "Sync session not found"}});

                auto replyShared = std::make_shared<RpcHelper::RpcOnce>(std::move(reply));
                boost::asio::post(
                    session->strand(),
                    [weak, session, options, loadedParents, syncSessionId, replyShared]() mutable
                    {
                        const auto snapshot = session->recomputeSnapshotInStrand(
                            options,
                            loadedParents,
                            [weakParent = weak, syncSessionId](std::uint64_t compared)
                            {
                                auto parent = weakParent.lock();
                                if (!parent)
                                    return;
                                // Hop back to the window/JS thread before emitting the RPC.
                                parent->wnd_->runInJavascriptThread(
                                    [parent, syncSessionId, compared]()
                                    {
                                        // Matches the OperationQueue::rpcName scheme
                                        // the frontend OperationQueue listens on.
                                        RpcHelper::callRemoteSafely(
                                            *parent->hub_,
                                            fmt::format("OperationQueue::{}::onSyncDiffProgress", parent->id_.value()),
                                            syncSessionId,
                                            std::to_string(compared)
                                        );
                                    }
                                );
                            }
                        );

                        nlohmann::json payload;
                        SharedData::to_json(payload, snapshot);
                        (*replyShared)(std::move(payload));
                    }
                );
            }
        );
}

void Session::registerRpcSftpLoadSyncDiffChildren()
{
    on(fmt::format("Session::{}::sftp::loadSyncDiffChildren", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& syncSessionIdString,
                SharedData::Sync::DiffSection section,
                std::string const& parentRelKey,
                std::string const& expectedGenerationString
            )
            {
                auto self = weak.lock();
                if (!self || !self->operationQueue_)
                    return reply({{"error", "Session no longer exists"}});

                const auto syncSessionId = Ids::makeSyncSessionId(syncSessionIdString);
                auto session = self->operationQueue_->syncSession(syncSessionId);
                if (!session)
                    return reply({{"error", "Sync session not found"}});

                const std::uint64_t expectedGeneration = std::stoull(expectedGenerationString);

                auto replyShared = std::make_shared<RpcHelper::RpcOnce>(std::move(reply));
                boost::asio::post(
                    session->strand(),
                    [session, section, parentRelKey, expectedGeneration, replyShared]()
                    {
                        if (session->generation() != expectedGeneration)
                        {
                            replyShared->error("stale generation");
                            return;
                        }
                        const auto children = session->loadChildrenInStrand(section, parentRelKey);
                        nlohmann::json payload = nlohmann::json::array();
                        for (auto const& node : children)
                        {
                            nlohmann::json entry;
                            SharedData::to_json(entry, node);
                            payload.push_back(std::move(entry));
                        }
                        (*replyShared)(std::move(payload));
                    }
                );
            }
        );
}

void Session::registerRpcSftpExecuteSyncSelection()
{
    // Plans the selection and resolves every row below the sync roots of the session.
    on(fmt::format("Session::{}::sftp::executeSyncSelection", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, nlohmann::json const& requestJson
            )
            {
                auto self = weak.lock();
                if (!self || !self->operationQueue_)
                    return reply.error("Session no longer exists");

                auto parsed =
                    RpcHelper::parseRequest<SharedData::Sync::SyncExecutionRequest>(requestJson, "sync request");
                if (!parsed)
                    return reply.error(parsed.error());
                auto request = *std::move(parsed);

                auto session = self->operationQueue_->syncSession(request.syncSessionId);
                if (!session)
                    return reply.error("Sync session not found");

                auto replyShared = std::make_shared<RpcHelper::RpcOnce>(std::move(reply));
                boost::asio::post(
                    session->strand(),
                    [weak, session, request = std::move(request), channelIdString, replyShared]() mutable
                    {
                        if (session->generation() != request.generation)
                            return replyShared->error(Utility::localizedMessage("backend.sync.staleComparison"));
                        auto self = weak.lock();
                        if (!self)
                            return replyShared->error("Session no longer exists");

                        auto plan = session->resolvePlanInStrand(
                            request.section, std::unordered_set<std::string>{request.relKeys.begin(), request.relKeys.end()}
                        );
                        self->withSftpChannelDo(
                            Ids::makeChannelId(channelIdString),
                            [session, plan = std::move(plan), request = std::move(request)](
                                RpcHelper::RpcOnce&& reply,
                                Session& self,
                                std::shared_ptr<SecureShell::SftpSession> const& channel
                            ) mutable
                            {
                                // A stopped session has no queue any more.
                                if (!self.operationQueue_)
                                    return reply.error("Session no longer exists");
                                auto result = self.operationQueue_->executeSyncPlan(
                                    *channel, *session, std::move(plan), request, *self.localOperationQueue_
                                );
                                self.wakeOperationQueue();
                                nlohmann::json json;
                                SharedData::to_json(json, result);
                                reply(std::move(json));
                            },
                            std::move(*replyShared)
                        );
                    }
                );
            }
        );
}

void Session::registerRpcSftpCancelSyncDiff()
{
    on(fmt::format("Session::{}::sftp::cancelSyncDiff", id_.value()))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, std::string const& syncSessionIdString)
            {
                auto self = weak.lock();
                if (!self || !self->operationQueue_)
                    return reply({{"error", "Session no longer exists"}});

                const auto syncSessionId = Ids::makeSyncSessionId(syncSessionIdString);
                if (auto session = self->operationQueue_->syncSession(syncSessionId); session)
                    session->cancel();
                reply({{"success", true}});
            }
        );
}

void Session::registerRpcSftpCloseSyncSession()
{
    on(fmt::format("Session::{}::sftp::closeSyncSession", id_.value()))
        .perform(
            [weak = weak_from_this()](RpcHelper::RpcOnce&& reply, std::string const& syncSessionIdString)
            {
                auto self = weak.lock();
                if (!self || !self->operationQueue_)
                    return reply({{"error", "Session no longer exists"}});

                self->operationQueue_->closeSyncSession(Ids::makeSyncSessionId(syncSessionIdString));
                reply({{"success", true}});
            }
        );
}

void Session::registerRpcSftpPreDeleteChecks()
{
    on(fmt::format("Session::{}::sftp::preDeleteChecks", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::vector<std::string> const& directories
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [directories](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto remotePaths = RpcHelper::parseRemotePaths(directories);
                        if (!remotePaths)
                            return reply.error(remotePaths.error());
                        auto fut = channel->filterOutEmptyDirectories(*std::move(remotePaths));
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Failed to perform pre-delete checks: timeout");

                        const auto result = fut.get();
                        if (!result.has_value())
                            return reply.error("Failed to perform pre-delete checks: " + result.error().toString());
                        reply({{"success", true}, {"nonEmptyDirectories", *result}});
                    },
                    std::move(reply)
                );
            }
        );
}
void Session::registerRpcSftpDeleteFiles()
{
    on(fmt::format("Session::{}::sftp::deleteFiles", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::vector<std::string> const& paths
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [paths](
                        RpcHelper::RpcOnce&& reply,
                        Session& self,
                        std::shared_ptr<SecureShell::SftpSession> const& channel
                    )
                    {
                        if (!self.operationQueue_)
                            return reply.error("Session no longer exists");
                        auto remotePaths = RpcHelper::parseRemotePaths(paths);
                        if (!remotePaths)
                            return reply.error(remotePaths.error());
                        auto guard = self.operationQueue_->remoteDeleteGuard(*channel);
                        if (const auto refusal = refuseAnyProtected(guard, *remotePaths); refusal)
                            return replyRefusal(reply, *refusal);
                        auto fut = channel->removeAll(*std::move(remotePaths));
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Failed to delete files: timeout");

                        const auto result = fut.get();
                        if (!result.has_value())
                            return reply.error("Failed to delete files: " + result.error().toString());

                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}
void Session::registerRpcSftpRename()
{
    on(fmt::format("Session::{}::sftp::rename", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& oldName,
                std::string const& newName,
                bool overwrite
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [oldName, newName, overwrite](
                        RpcHelper::RpcOnce&& reply,
                        Session& self,
                        std::shared_ptr<SecureShell::SftpSession> const& channel
                    )
                    {
                        const auto oldPath = Paths::RemotePath::make(oldName);
                        const auto newPath = Paths::RemotePath::make(newName);
                        if (!oldPath || !newPath || !oldPath->parent() || oldPath->parent() != newPath->parent())
                            return reply.error("backend.rename.invalidName");

                        auto fut = channel->performPromise(
                            [session = std::weak_ptr{channel},
                                oldPath = *oldPath,
                                newPath = *newPath,
                                overwrite,
                                asideSuffix = Persistence::asideFileSuffix(self.remoteTempFileSuffix_)]()
                                -> std::expected<void, SecureShell::SftpError>
                            {
                                const auto sftp = session.lock();
                                if (!sftp)
                                    return std::unexpected(
                                        SecureShell::SftpError{.message = "The SFTP channel was closed"}
                                    );
                                return sftp->moveIntoPlaceInStrand(oldPath, newPath, overwrite, asideSuffix);
                            }
                        );
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Failed to rename file: timeout");

                        const auto result = fut.get();
                        if (!result.has_value())
                        {
                            if (result.error().sftpError == SSH_FX_FILE_ALREADY_EXISTS)
                                return reply.error("backend.rename.targetExists");
                            return reply.error("Failed to rename file: " + result.error().toString());
                        }
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpAddBulkDeleteOperation()
{
    // One RPC and one card for the whole bulk delete, under the operation id the frontend reserved.
    on(fmt::format("Session::{}::sftp::addBulkDelete", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& bulkOperationIdString,
                nlohmann::json const& requestJson
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                auto request = RpcHelper::parseRequest<SharedData::BulkDeleteRequest>(requestJson, "bulk delete");
                if (!request)
                    return reply.error(request.error());

                auto description = fmt::format("the bulk delete of {} entries", request->entries.size());
                self->queueOnChannel(
                    std::move(reply),
                    Ids::makeChannelId(channelIdString),
                    std::move(description),
                    [operationId = Ids::makeOperationId(bulkOperationIdString), request = *std::move(request)](
                        OperationQueue& queue, SecureShell::SftpSession& channel
                    ) -> std::expected<nlohmann::json, std::string>
                    {
                        auto guard = queue.remoteDeleteGuard(channel);
                        if (const auto refusal =
                                refuseAnyProtected(guard, request.entries, &SharedData::BulkDeleteEntry::target);
                            refusal)
                            return std::unexpected(refusal->toLocalizedMessage());
                        return enqueuedReply(queue.addBulkDeleteOperation(guard, channel, request, operationId));
                    }
                );
            }
        );
}

void Session::registerRpcAddLocalDeleteOperation()
{
    on(fmt::format("Session::{}::addLocalDelete", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& operationIdString, std::vector<std::string> const& paths
            )
            {
                auto self = weak.lock();
                if (!self || !self->operationQueue_)
                    return reply.error("Session no longer exists");
                const auto added = self->localOperationQueue_->addExplorerDelete(
                    Ids::makeOperationId(operationIdString), paths, self->id_, self->operationQueue_->paused()
                );
                if (!added)
                    return reply.error(added.error());
                reply(SharedData::success());
            }
        );
}

void Session::registerRpcSftpExistsBatch()
{
    // Whether each of N remote paths exists, in one RPC. Only "no such file" is absent, any other failure fails the
    // whole batch: the frontend overwrites what it takes for absent.
    on(fmt::format("Session::{}::sftp::existsBatch", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::vector<std::string> const& paths
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [paths](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto results = remotePathsExist(*channel, paths, futureTimeout);
                        if (!results)
                        {
                            Log::error("sftp::existsBatch: {}", results.error());
                            return reply.error(results.error());
                        }
                        Log::info("sftp::existsBatch: probed {} paths", paths.size());
                        reply({{"success", true}, {"exists", *results}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpStat()
{
    on(fmt::format("Session::{}::sftp::stat", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& path
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply.error("Session no longer exists");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [path](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        const auto remotePath = RpcHelper::parseRemotePath(path);
                        if (!remotePath)
                            return reply.error(remotePath.error());
                        auto fut = channel->stat(*remotePath);
                        if (fut.wait_for(futureTimeout) != std::future_status::ready)
                            return reply.error("Failed to stat file: timeout");

                        const auto result = fut.get();
                        if (!result.has_value())
                        {
                            if (result.error().sftpError == SSH_FX_NO_SUCH_FILE)
                            {
                                // File does not exist, return nullopt
                                return reply({{"success", true}, {"stat", nullptr}});
                            }
                            return reply.error("Failed to stat file: " + result.error().toString());
                        }
                        reply({{"success", true}, {"stat", *result}});
                    },
                    std::move(reply)
                );
            }
        );
}