#include <backend/session.hpp>
#include <backend/existence_check.hpp>

#include <utility/path_utf.hpp>
#include <paths/paths.hpp>

#include <roar/utility/base64.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/file_operations/bulk_delete_request.hpp>
#include <shared_data/sync/sync_execution.hpp>

#include <nui/utility/scope_exit.hpp>

#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace std::chrono_literals;

namespace
{
    /**
     * @brief A remote path from the frontend, or the message to reply with when it is not a valid absolute path.
     */
    std::expected<Paths::RemotePath, std::string> parseRemotePath(std::string const& path)
    {
        return Paths::RemotePath::make(path).transform_error(
            [](Paths::PathError const& error)
            {
                return fmt::format("Invalid remote path: {}", error.toString());
            }
        );
    }

    /**
     * @brief A local path from the frontend, or the message to reply with when it is not a valid absolute path.
     */
    std::expected<Paths::LocalPath, std::string> parseLocalPath(std::string const& path)
    {
        return Paths::LocalPath::fromUtf8(path).transform_error(
            [](Paths::PathError const& error)
            {
                return fmt::format("Invalid local path: {}", error.toString());
            }
        );
    }

    /**
     * @brief A JSON request from the frontend, or the message to reply with when it does not parse. Paths are
     *        validated while parsing.
     */
    template <typename RequestT>
    std::expected<RequestT, std::string> parseRequest(nlohmann::json const& json, std::string_view what)
    {
        try
        {
            return json.get<RequestT>();
        }
        catch (std::exception const& exception)
        {
            return std::unexpected(fmt::format("Invalid {}: {}", what, exception.what()));
        }
    }

    std::expected<std::vector<Paths::RemotePath>, std::string> parseRemotePaths(std::vector<std::string> const& paths)
    {
        std::vector<Paths::RemotePath> result;
        result.reserve(paths.size());
        for (auto const& path : paths)
        {
            auto parsed = parseRemotePath(path);
            if (!parsed)
                return std::unexpected(std::move(parsed).error());
            result.push_back(*std::move(parsed));
        }
        return result;
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
    BulkResumeRegistry* bulkResumeRegistry
)
    : RpcHelper::StrandRpc{executor, std::move(strand), wnd, hub}
    , id_{std::move(id)}
    , session_{std::move(session)}
    , operationQueue_{std::make_shared<
          OperationQueue>(executor_, strand_, wnd, hub, sftpOptions, id_, sftpOptions.concurrency.value_or(Persistence::defaultSftpConcurrency))}
    , bulkResumeRegistry_{bulkResumeRegistry}
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

            Log::info("Session '{}' connected", self->id_.value());

            self->running_ = true;
            self->doOperationQueueWork();
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

            // Backup any in-flight or queued bulk operations before
            // teardown so a seamless reconnect can adopt them.  Always
            // happens — the frontend's adopt/discard signals (and the
            // registry TTL) handle cleanup if no resume occurs.
            if (self->bulkResumeRegistry_)
                self->operationQueue_->dumpBulkResumes(*self->bulkResumeRegistry_);
            self->operationQueue_->cancelAll();
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
            std::shared_ptr<SecureShell::SftpSession> sftp;
            for (auto const& [channelId, weakChannel] : self->sftpChannels_)
            {
                if (auto locked = weakChannel.lock(); locked)
                {
                    sftp = locked;
                    break;
                }
            }
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

void Session::doOperationQueueWork()
{
    within_strand_do_no_recurse(
        [weak = weak_from_this()]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            if (!self->running_)
                return;

            if (self->operationQueue_ &&
                (!self->operationQueue_->paused() || self->operationQueue_->hasPriorityWork()) &&
                self->operationQueue_->work())
            {
                ++self->unthrottledLimitCounter_;
                if (self->unthrottledLimitCounter_ < 10)
                {
                    self->doOperationQueueWork();
                    self->operationThrottle_ = queueStartThrottle;
                    return;
                }
                self->unthrottledLimitCounter_ = 0;
            }

            if (self->operationThrottle_ < queueMaxThrottle)
                self->operationThrottle_ *= 2;
            else
                self->operationThrottle_ = queueMaxThrottle;

            self->queueThrottleTimerIsRunning_ = true;
            self->within_strand_do_delayed(
                [weak = self->weak_from_this()]()
                {
                    if (auto self = weak.lock(); self)
                    {
                        self->queueThrottleTimerIsRunning_ = false;
                        self->doOperationQueueWork();
                    }
                },
                self->operationThrottle_
            );
        }
    );
}

void Session::resetQueueThrottle()
{
    within_strand_do(
        [weak = weak_from_this()]()
        {
            auto self = weak.lock();
            if (!self)
                return;

            self->operationThrottle_ = queueStartThrottle;
            if (self->queueThrottleTimerIsRunning_)
                self->timer_.cancel();
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
                        const auto remotePath = parseRemotePath(path);
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
                        const auto homePath = parseRemotePath(home);
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
                                {{"name", directory.name}, {"path", Utility::pathToUtf8Generic(directory.path)}}
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

                auto parsed = parseRemotePaths(pathStrings);
                if (!parsed)
                    return reply.error(parsed.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(), paths = *std::move(parsed), recursive, insertRefresh, mode](
                        RpcHelper::RpcOnce&& reply, auto&& channel
                    ) mutable
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply.error("Session no longer exists");

                        // Nothing is queued when one of the targets is protected.
                        for (auto const& path : paths)
                        {
                            if (auto allowed = self->operationQueue_->refuseProtectedDeleteTarget(*channel, path);
                                !allowed)
                                return reply.error(fmt::format("Refusing to delete '{}'", path.value()));
                        }

                        std::vector<std::string> operationIds;
                        for (auto const& path : paths)
                        {
                            const auto newOperationId = Ids::generateOperationId();
                            const auto result = self->operationQueue_->addDeleteOperation(
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
                        const auto remotePath = parseRemotePath(path);
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
                        const auto remotePath = parseRemotePath(path);
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
                    return reply({{"error", "Session no longer exists"}});

                auto remote = parseRemotePath(remotePath);
                if (!remote)
                    return reply.error(remote.error());
                auto local = parseLocalPath(localPath);
                if (!local)
                    return reply.error(local.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        newOperationIdString,
                        local = *std::move(local),
                        remote = *std::move(remote),
                        allowOverwrite,
                        bigFile,
                        insertRefresh,
                        createMissingDirectories,
                        mode](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        const auto result = self->operationQueue_->addDownloadOperation(
                            *channel,
                            Ids::makeOperationId(newOperationIdString),
                            local,
                            remote,
                            allowOverwrite,
                            bigFile,
                            insertRefresh,
                            createMissingDirectories,
                            static_cast<SharedData::OperationMode>(mode)
                        );

                        if (!result.has_value())
                        {
                            Log::error(
                                "Failed to add download operation for file '{}' to '{}': {}",
                                remote.value(),
                                local.toUtf8(),
                                result.error().toString()
                            );
                            return reply.error("Failed to add download operation: " + result.error().toString());
                        }

                        Log::info(
                            "Added download operation with id '{}' for file '{}' to '{}'",
                            newOperationIdString,
                            remote.value(),
                            local.toUtf8()
                        );

                        self->resetQueueThrottle();
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpAddArchiveDownloadOperation()
{
    // One RPC to pack N remote files into a single local tar[.ext] archive.
    // The entries list is trusted (no per-entry lstat) and the operation runs
    // sequentially on the SFTP strand like a regular DownloadOperation.
    on(fmt::format("Session::{}::sftp::addArchiveDownload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::string const& newOperationIdString,
                std::string const& entriesJson,
                std::string const& localArchivePath,
                int compressionCodec,
                int compressionLevel,
                bool mayOverwrite,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                std::vector<SharedData::DirectoryEntry> entries;
                try
                {
                    entries = nlohmann::json::parse(entriesJson).get<std::vector<SharedData::DirectoryEntry>>();
                }
                catch (std::exception const& exc)
                {
                    Log::error("addArchiveDownload: failed to parse entries JSON: {}", exc.what());
                    return reply({{"error", std::string{"Invalid entries payload: "} + exc.what()}});
                }
                auto archivePath = parseLocalPath(localArchivePath);
                if (!archivePath)
                    return reply.error(archivePath.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        newOperationIdString,
                        entries = std::move(entries),
                        localArchivePath = *std::move(archivePath),
                        compressionCodec,
                        compressionLevel,
                        mayOverwrite,
                        mode](RpcHelper::RpcOnce&& reply, auto&& channel) mutable
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        const auto codec = static_cast<TarArchive::Compression>(compressionCodec);

                        const auto result = self->operationQueue_->addArchiveDownloadOperation(
                            *channel,
                            Ids::makeOperationId(newOperationIdString),
                            std::move(entries),
                            localArchivePath,
                            codec,
                            compressionLevel,
                            mayOverwrite,
                            static_cast<SharedData::OperationMode>(mode)
                        );

                        if (!result.has_value())
                        {
                            Log::error(
                                "Failed to add archive-download operation to '{}': {}",
                                localArchivePath.toUtf8(),
                                result.error().toString()
                            );
                            return reply.error(
                                "Failed to add archive-download operation: " + result.error().toString()
                            );
                        }

                        Log::info(
                            "Added archive-download operation '{}' → '{}' ({} entries)",
                            newOperationIdString,
                            localArchivePath.toUtf8(),
                            entries.size()
                        );

                        self->resetQueueThrottle();
                        reply({{"success", true}});
                    },
                    std::move(reply)
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
                bool mayOverwrite,
                int mode
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                std::vector<Paths::LocalPath> localPaths;
                localPaths.reserve(localPathStrings.size());
                for (auto const& pathString : localPathStrings)
                {
                    auto localPath = parseLocalPath(pathString);
                    if (!localPath)
                        return reply.error(localPath.error());
                    localPaths.push_back(*std::move(localPath));
                }
                auto archivePath = parseRemotePath(remoteArchivePath);
                if (!archivePath)
                    return reply.error(archivePath.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        newOperationIdString,
                        localPaths = std::move(localPaths),
                        remoteArchivePath = *std::move(archivePath),
                        compressionCodec,
                        compressionLevel,
                        mayOverwrite,
                        mode](RpcHelper::RpcOnce&& reply, auto&& channel) mutable
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        const auto codec = static_cast<TarArchive::Compression>(compressionCodec);
                        const auto result = self->operationQueue_->addArchiveUploadOperation(
                            *channel,
                            Ids::makeOperationId(newOperationIdString),
                            std::move(localPaths),
                            remoteArchivePath,
                            codec,
                            compressionLevel,
                            mayOverwrite,
                            static_cast<SharedData::OperationMode>(mode)
                        );
                        if (!result.has_value())
                        {
                            Log::error(
                                "Failed to add archive-upload operation to '{}': {}",
                                remoteArchivePath.value(),
                                result.error().toString()
                            );
                            return reply.error("Failed to add archive-upload operation: " + result.error().toString());
                        }
                        Log::info(
                            "Added archive-upload operation '{}' → '{}'",
                            newOperationIdString,
                            remoteArchivePath.value()
                        );
                        self->resetQueueThrottle();
                        reply({{"success", true}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpAddBulkDownloadOperation()
{
    // One RPC for a whole batch. The handler performs all queueing inside a
    // single strand dispatch so the SSH thread-hop cost is paid once per
    // batch instead of once per file. Files are trusted (no lstat per entry).
    on(fmt::format("Session::{}::sftp::addBulkDownload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::vector<std::string> const& operationIdStrings,
                nlohmann::json const& requestJson
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                // Parsed here, so an invalid path is answered with an error instead of no reply at all.
                auto parsed = parseRequest<SharedData::BulkDownloadRequest>(requestJson, "bulk download");
                if (!parsed)
                    return reply.error(parsed.error());
                auto request = *std::move(parsed);

                // Expect N per-entry ids + 1 dedicated aggregate-bulk-card id at the end.
                if (operationIdStrings.size() != request.entries.size() + 1)
                    return reply.error("addBulkDownload: operationIds and entries length mismatch");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        operationIdStrings,
                        request](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        const auto bulkCardId = Ids::makeOperationId(operationIdStrings.back());
                        const auto enqueued = self->operationQueue_->addBulkDownloadOperation(
                            *channel,
                            request,
                            [&operationIdStrings](std::size_t idx)
                            {
                                return Ids::makeOperationId(operationIdStrings[idx]);
                            },
                            bulkCardId
                        );

                        Log::info("addBulkDownload: queued {}/{} entries", enqueued, request.entries.size());

                        self->resetQueueThrottle();
                        reply({{"success", true}, {"enqueued", enqueued}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpAddBulkUploadOperation()
{
    // See registerRpcSftpAddBulkDownloadOperation — same contract for upload.
    on(fmt::format("Session::{}::sftp::addBulkUpload", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply,
                std::string const& channelIdString,
                std::vector<std::string> const& operationIdStrings,
                nlohmann::json const& requestJson
            )
            {
                auto self = weak.lock();
                if (!self)
                    return reply({{"error", "Session no longer exists"}});

                // Parsed here, so an invalid path is answered with an error instead of no reply at all.
                auto parsed = parseRequest<SharedData::BulkUploadRequest>(requestJson, "bulk upload");
                if (!parsed)
                    return reply.error(parsed.error());
                auto request = *std::move(parsed);

                if (operationIdStrings.size() != request.entries.size() + 1)
                    return reply.error("addBulkUpload: operationIds and entries length mismatch");

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        operationIdStrings,
                        request](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        const auto bulkCardId = Ids::makeOperationId(operationIdStrings.back());
                        const auto enqueued = self->operationQueue_->addBulkUploadOperation(
                            *channel,
                            request,
                            [&operationIdStrings](std::size_t idx)
                            {
                                return Ids::makeOperationId(operationIdStrings[idx]);
                            },
                            bulkCardId
                        );

                        Log::info("addBulkUpload: queued {}/{} entries", enqueued, request.entries.size());
                        self->resetQueueThrottle();
                        reply({{"success", true}, {"enqueued", enqueued}});
                    },
                    std::move(reply)
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
                    return reply({{"error", "Session no longer exists"}});

                auto local = parseLocalPath(localPath);
                if (!local)
                    return reply.error(local.error());
                auto remote = parseRemotePath(remotePath);
                if (!remote)
                    return reply.error(remote.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        newOperationIdString,
                        local = *std::move(local),
                        remote = *std::move(remote),
                        allowOverwrite,
                        bigFile,
                        insertRefresh,
                        createMissingDirectories,
                        mode](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        const auto result = self->operationQueue_->addUploadOperation(
                            *channel,
                            Ids::makeOperationId(newOperationIdString),
                            local,
                            remote,
                            allowOverwrite,
                            bigFile,
                            insertRefresh,
                            createMissingDirectories,
                            static_cast<SharedData::OperationMode>(mode)
                        );

                        if (!result.has_value())
                        {
                            Log::error(
                                "Failed to add upload operation for file '{}' to '{}': {}",
                                local.toUtf8(),
                                remote.value(),
                                result.error().toString()
                            );
                            return reply.error("Failed to add upload operation: " + result.error().toString());
                        }

                        Log::info(
                            "Added upload operation with id '{}' for file '{}' to '{}'",
                            newOperationIdString,
                            local.toUtf8(),
                            remote.value()
                        );

                        self->resetQueueThrottle();
                        reply({{"success", true}});
                    },
                    std::move(reply)
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
                    return reply({{"error", "Session no longer exists"}});

                auto source = parseRemotePath(sourcePath);
                if (!source)
                    return reply.error(source.error());
                auto destination = parseRemotePath(destinationPath);
                if (!destination)
                    return reply.error(destination.error());

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        newOperationIdString,
                        source = *std::move(source),
                        destination = *std::move(destination),
                        mode](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        const auto result = self->operationQueue_->addRenameOperation(
                            *channel,
                            Ids::makeOperationId(newOperationIdString),
                            source,
                            destination,
                            static_cast<SharedData::OperationMode>(mode)
                        );

                        if (!result.has_value())
                        {
                            Log::error(
                                "Failed to add rename operation for '{}' to '{}': {}",
                                source.value(),
                                destination.value(),
                                result.error().toString()
                            );
                            return reply.error("Failed to add rename operation: " + result.error().toString());
                        }

                        Log::info(
                            "Added rename operation with id '{}' for '{}' to '{}'",
                            newOperationIdString,
                            source.value(),
                            destination.value()
                        );

                        self->resetQueueThrottle();
                        reply({{"success", true}});
                    },
                    std::move(reply)
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
                self->resetQueueThrottle();
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
                auto remoteRoot = parseRemotePath(remotePath);
                if (!remoteRoot)
                    return reply.error(remoteRoot.error());
                auto localRoot = Paths::LocalPath::fromUtf8(localPath);
                if (!localRoot)
                    return reply.error(fmt::format("Invalid local path: {}", localRoot.error().toString()));

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        syncSessionIdString,
                        remoteScanIdString,
                        localScanIdString,
                        remotePath = *std::move(remoteRoot),
                        localPath = *std::move(localRoot),
                        respectIgnoreFiles,
                        recursive,
                        ignoreHidden](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        self->operationQueue_->addSyncScanOperation(
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

                        self->resetQueueThrottle();
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
                                        parent->hub_->callRemote(
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
    // The backend plans the selection and resolves every row below its own sync roots, so no absolute path
    // round-trips through the frontend.
    on(fmt::format("Session::{}::sftp::executeSyncSelection", id_.value()))
        .perform(
            [weak = weak_from_this()](
                RpcHelper::RpcOnce&& reply, std::string const& channelIdString, nlohmann::json const& requestJson
            )
            {
                auto self = weak.lock();
                if (!self || !self->operationQueue_)
                    return reply.error("Session no longer exists");

                SharedData::Sync::SyncExecutionRequest request{};
                try
                {
                    request = requestJson.get<SharedData::Sync::SyncExecutionRequest>();
                }
                catch (std::exception const& exception)
                {
                    return reply.error(fmt::format("Invalid sync request: {}", exception.what()));
                }

                auto session = self->operationQueue_->syncSession(request.syncSessionId);
                if (!session)
                    return reply.error("Sync session not found");

                auto replyShared = std::make_shared<RpcHelper::RpcOnce>(std::move(reply));
                boost::asio::post(
                    session->strand(),
                    [self, session, request = std::move(request), channelIdString, replyShared]() mutable
                    {
                        if (session->generation() != request.generation)
                            return replyShared->error("stale generation");

                        auto plan = session->resolvePlanInStrand(
                            request.section, std::unordered_set<std::string>{request.relKeys.begin(), request.relKeys.end()}
                        );
                        self->withSftpChannelDo(
                            Ids::makeChannelId(channelIdString),
                            [self, session, plan = std::move(plan), request = std::move(request)](
                                RpcHelper::RpcOnce&& reply, auto&& channel
                            ) mutable
                            {
                                auto result = self->operationQueue_->executeSyncPlan(
                                    *channel, *session, std::move(plan), request
                                );
                                self->resetQueueThrottle();
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
                    [weak = self->weak_from_this(), directories](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply.error("Session no longer exists");

                        auto remotePaths = parseRemotePaths(directories);
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
                    [weak = self->weak_from_this(), paths](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply.error("Session no longer exists");

                        auto remotePaths = parseRemotePaths(paths);
                        if (!remotePaths)
                            return reply.error(remotePaths.error());
                        for (auto const& path : *remotePaths)
                        {
                            if (auto allowed = self->operationQueue_->refuseProtectedDeleteTarget(*channel, path);
                                !allowed)
                                return reply.error(fmt::format("Refusing to delete '{}'", path.value()));
                        }
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
                    [weak = self->weak_from_this(), oldName, newName, overwrite](
                        RpcHelper::RpcOnce&& reply, auto&& channel
                    )
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply.error("Session no longer exists");

                        const auto oldPath = Paths::RemotePath::make(oldName);
                        const auto newPath = Paths::RemotePath::make(newName);
                        if (!oldPath || !newPath || !oldPath->parent() || oldPath->parent() != newPath->parent())
                            return reply.error("backend.rename.invalidName");

                        // Never replaces an existing item unless the user confirmed it.
                        auto fut = channel->performPromise(
                            [channel, oldPath = *oldPath, newPath = *newPath, overwrite]()
                            {
                                return overwrite ? channel->renameReplaceInStrand(oldPath, newPath, ".nui-sftp-old")
                                                 : channel->renameNoReplaceInStrand(oldPath, newPath);
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
    // One RPC for the whole bulk delete.  Frontend pre-allocates a single
    // OperationId for the aggregate file-bulk card; the backend may also
    // create extra single-Delete cards for any directory entries.
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
                    return reply({{"error", "Session no longer exists"}});

                // Parsed here, so an invalid target is answered with an error instead of no reply at all.
                SharedData::BulkDeleteRequest request{};
                try
                {
                    request = requestJson.get<SharedData::BulkDeleteRequest>();
                }
                catch (std::exception const& exception)
                {
                    return reply.error(fmt::format("Invalid bulk delete: {}", exception.what()));
                }

                self->withSftpChannelDo(
                    Ids::makeChannelId(channelIdString),
                    [weak = self->weak_from_this(),
                        bulkOperationIdString,
                        request](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply({{"error", "Session no longer exists"}});

                        // Nothing is queued when one of the targets is protected.
                        for (auto const& entry : request.entries)
                        {
                            if (auto allowed = self->operationQueue_->refuseProtectedDeleteTarget(*channel, entry.target);
                                !allowed)
                                return reply.error(fmt::format("Refusing to delete '{}'", entry.target.value()));
                        }

                        const auto enqueued = self->operationQueue_->addBulkDeleteOperation(
                            *channel, request, Ids::makeOperationId(bulkOperationIdString)
                        );

                        Log::info("addBulkDelete: queued {} entries", enqueued);
                        self->resetQueueThrottle();
                        reply({{"success", true}, {"enqueued", enqueued}});
                    },
                    std::move(reply)
                );
            }
        );
}

void Session::registerRpcSftpExistsBatch()
{
    // Batched existence probe over SFTP.  Lets the frontend ask whether N
    // remote paths exist in a single round-trip — eliminates the per-file
    // SFTP stat latency that made multi-file uploads feel slow.  Only "no such
    // file" is absent, any other failure fails the whole batch: the frontend
    // overwrites what it takes for absent.
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
                    [weak = self->weak_from_this(), path](RpcHelper::RpcOnce&& reply, auto&& channel)
                    {
                        auto self = weak.lock();
                        if (!self)
                            return reply.error("Session no longer exists");

                        const auto remotePath = parseRemotePath(path);
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