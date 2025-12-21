#include <backend/session.hpp>

#include <roar/utility/base64.hpp>
#include <shared_data/error_or_success.hpp>

using namespace std::chrono_literals;

Session::Session(
    Ids::SessionId id,
    std::unique_ptr<SecureShell::Session> session,
    boost::asio::any_io_executor executor,
    std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    Persistence::SftpOptions const& sftpOptions)
    : RpcHelper::StrandRpc{executor, std::move(strand), wnd, hub}
    , id_{std::move(id)}
    , session_{std::move(session)}
    , operationQueue_{std::make_shared<
          OperationQueue>(executor_, strand_, wnd, hub, sftpOptions, id_, sftpOptions.concurrency.value_or(1))}
{}

void Session::start()
{
    within_strand_do([weak = weak_from_this()]() {
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
        self->registerRpcSftpCreateDirectory();
        self->registerRpcSftpCreateFile();
        self->registerRpcSftpAddDownloadOperation();
        self->registerRpcSftpAddUploadOperation();
        self->registerOperationQueuePauseUnpause();
        self->operationQueue_->registerRpc();

        Log::info("Session '{}' connected", self->id_.value());

        self->running_ = true;
        self->doOperationQueueWork();
    });
}

void Session::stop()
{
    running_ = false;
    timer_.cancel();
}

void Session::doOperationQueueWork()
{
    within_strand_do_no_recurse([weak = weak_from_this()]() {
        auto self = weak.lock();
        if (!self)
            return;

        if (!self->running_)
            return;

        if (self->operationQueue_ && !self->operationQueue_->paused() && self->operationQueue_->work())
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
            [weak = self->weak_from_this()]() {
                if (auto self = weak.lock(); self)
                {
                    self->queueThrottleTimerIsRunning_ = false;
                    self->doOperationQueueWork();
                }
            },
            self->operationThrottle_);
    });
}

void Session::resetQueueThrottle()
{
    within_strand_do([weak = weak_from_this()]() {
        auto self = weak.lock();
        if (!self)
            return;

        self->operationThrottle_ = queueStartThrottle;
        if (self->queueThrottleTimerIsRunning_)
            self->timer_.cancel();
    });
}

void Session::removeChannel(Ids::ChannelId channelId)
{
    within_strand_do([weak = weak_from_this(), channelId = std::move(channelId)]() {
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
    });
}

void Session::removeSftpChannel(Ids::ChannelId channelId)
{
    within_strand_do([weak = weak_from_this(), channelId = std::move(channelId)]() {
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
    });
}

void Session::registerRpcCreateChannel()
{
    on(fmt::format("Session::{}::Channel::create", id_.value()))
        .perform([weak = weak_from_this()](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            RpcHelper::ParameterVerifyView verify{
                reply, fmt::format("Session::{}::Channel::create", self->id_.value()), parameters};

            if (!verify.hasValueDeep("engine"))
                return;

            const bool fileMode = parameters.contains("fileMode") && parameters["fileMode"].is_boolean() &&
                parameters["fileMode"].get<bool>();

            if (!fileMode)
            {
                Log::info("Creating pty channel for session '{}'", self->id_.value());

                const auto sessionOptions =
                    parameters["engine"]["sshSessionOptions"].get<Persistence::SshSessionOptions>();

                const auto weakChannel =
                    self->session_->createPtyChannel({.environment = sessionOptions.environment}).get();
                if (!weakChannel.has_value())
                {
                    Log::error("Failed to create pty channel: {}", weakChannel.error());
                    return reply({{"error", "Failed to create pty channel"}});
                }

                const auto channelId = Ids::generateChannelId();
                self->channels_.emplace(channelId, std::move(weakChannel).value());

                Log::info(
                    "Created pty channel with id '{}', channel total is now '{}'.",
                    channelId.value(),
                    self->channels_.size());

                return reply({{"id", channelId.value()}});
            }
            else
            {
                Log::info("Creating sftp channel for session '{}'", self->id_.value());

                const auto weakChannel = self->session_->createSftpSession().get();
                if (!weakChannel.has_value())
                {
                    Log::error("Failed to create sftp channel: {}", weakChannel.error().toString());
                    return reply({{"error", "Failed to create sftp channel"}});
                }

                const auto channelId = Ids::generateChannelId();
                self->sftpChannels_.emplace(channelId, std::move(weakChannel).value());

                Log::info(
                    "Created sftp channel with id '{}', sftp channel total is now '{}'.",
                    channelId.value(),
                    self->sftpChannels_.size());

                return reply({{"id", channelId.value()}});
            }
        });
}

void Session::registerRpcStartChannelRead()
{
    on(fmt::format("Session::{}::Channel::startReading", id_.value()))
        .perform([weak = weak_from_this()](RpcHelper::RpcOnce&& reply, std::string const& channelIdString) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            const auto channelId = Ids::makeChannelId(channelIdString);
            if (self->channels_.find(channelId) == self->channels_.end())
            {
                Log::error("No channel found with id: {}", channelId.value());
                return reply({{"error", "No channel found with id"}});
            }

            auto locked = self->channels_[channelId].lock();
            if (!locked)
            {
                Log::error("Failed to lock channel with id: {}", channelId.value());
                self->removeChannel(channelId);
                return reply({{"error", "Failed to lock channel"}});
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
                     }](std::string const& msg) {
                    stdOut(
                        nlohmann::json{
                            {"sessionId", sessionId.value()},
                            {"channelId", channelId.value()},
                            {"data", Roar::base64Encode(msg)},
                        });
                },
                // Stderr
                [sessionId = self->id_,
                 channelId,
                 stdErr =
                     RpcHelper::RpcInCorrectThread{
                         *self->wnd_,
                         *self->hub_,
                         fmt::format("sshTerminalStderr_{}", channelId.value()),
                     }](std::string const& data) {
                    stdErr(
                        nlohmann::json{
                            {"sessionId", sessionId.value()},
                            {"channelId", channelId.value()},
                            {"data", Roar::base64Encode(data)},
                        });
                },
                // On channel exit:
                [removeChannel =
                     [weak = self->weak_from_this(), channelId]() {
                         if (auto self = weak.lock(); self)
                             self->removeChannel(channelId);
                     },
                 sessionId = self->id_,
                 channelId,
                 onExit = RpcHelper::RpcInCorrectThread{
                     *self->wnd_,
                     *self->hub_,
                     fmt::format("sshTerminalOnExit_{}", channelId.value()),
                 }]() {
                    Log::info("Channel for session '{}' lost with id: {}", sessionId.value(), channelId.value());
                    removeChannel();
                    onExit({{"sessionId", sessionId.value()}, {"channelId", channelId.value()}});
                });

            return reply({{"success", true}});
        });
}

void Session::registerRpcChannelClose()
{
    on(fmt::format("Session::{}::Channel::close", id_.value()))
        .perform([weak = weak_from_this()](RpcHelper::RpcOnce&& reply, std::string const& channelIdString) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->removeChannel(Ids::makeChannelId(channelIdString));
            return reply({{"success", true}});
        });
}

void Session::registerRpcChannelWrite()
{
    on(fmt::format("Session::{}::Channel::write", id_.value()))
        .perform([weak = weak_from_this()](
                     RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string&& data) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->withChannelDo(
                Ids::makeChannelId(channelIdString),
                [data = std::move(data)](RpcHelper::RpcOnce&& reply, auto&& channel) {
                    channel->write(Roar::base64Decode(data));
                    reply({{"success", true}});
                },
                std::move(reply));
        });
}

void Session::registerRpcChannelPtyResize()
{
    on(fmt::format("Session::{}::Channel::ptyResize", id_.value()))
        .perform([weak = weak_from_this()](
                     RpcHelper::RpcOnce&& reply, std::string const& channelIdString, int cols, int rows) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->withChannelDo(
                Ids::makeChannelId(channelIdString),
                [cols, rows](RpcHelper::RpcOnce&& reply, auto&& channel) {
                    channel->resizePty(cols, rows);
                    reply({{"success", true}});
                },
                std::move(reply));
        });
}

void Session::registerRpcSftpListDirectory()
{
    on(fmt::format("Session::{}::sftp::listDirectory", id_.value()))
        .perform([weak = weak_from_this()](
                     RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& path) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->withSftpChannelDo(
                Ids::makeChannelId(channelIdString),
                [path](RpcHelper::RpcOnce&& reply, auto&& channel) {
                    auto fut = channel->listDirectory(path);
                    if (fut.wait_for(futureTimeout) != std::future_status::ready)
                        return reply({{"error", "Failed to list directory: timeout"}});

                    const auto result = fut.get();
                    if (!result.has_value())
                        return reply({{"error", result.error().message}});

                    Log::info("Listed directory '{}', got {} entries", path, result->size());
                    reply({{"entries", *result}});
                },
                std::move(reply));
        });
}

void Session::registerRpcSftpCreateDirectory()
{
    on(fmt::format("Session::{}::sftp::createDirectory", id_.value()))
        .perform([weak = weak_from_this()](
                     RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& path) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->withSftpChannelDo(
                Ids::makeChannelId(channelIdString),
                [path](RpcHelper::RpcOnce&& reply, auto&& channel) {
                    auto fut = channel->createDirectory(path);
                    if (fut.wait_for(futureTimeout) != std::future_status::ready)
                        return reply({{"error", "Failed to create directory: timeout"}});

                    const auto result = fut.get();
                    if (!result.has_value())
                        return reply({{"error", result.error().message}});

                    Log::info("Created directory '{}'", path);
                    reply({{"success", true}});
                },
                std::move(reply));
        });
}

void Session::registerRpcSftpCreateFile()
{
    on(fmt::format("Session::{}::sftp::createFile", id_.value()))
        .perform([weak = weak_from_this()](
                     RpcHelper::RpcOnce&& reply, std::string const& channelIdString, std::string const& path) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->withSftpChannelDo(
                Ids::makeChannelId(channelIdString),
                [path](RpcHelper::RpcOnce&& reply, auto&& channel) {
                    auto fut = channel->createFile(path);
                    if (fut.wait_for(futureTimeout) != std::future_status::ready)
                        return reply({{"error", "Failed to create file: timeout"}});

                    const auto result = fut.get();
                    if (!result.has_value())
                        return reply({{"error", result.error().message}});

                    Log::info("Created file '{}'", path);
                    reply({{"success", true}});
                },
                std::move(reply));
        });
}

void Session::registerRpcSftpAddDownloadOperation()
{
    on(fmt::format("Session::{}::sftp::addDownload", id_.value()))
        .perform([weak = weak_from_this()](
                     RpcHelper::RpcOnce&& reply,
                     std::string const& channelIdString,
                     std::string const& newOperationIdString,
                     std::string const& remotePath,
                     std::string const& localPath) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->withSftpChannelDo(
                Ids::makeChannelId(channelIdString),
                [weak = self->weak_from_this(), newOperationIdString, localPath, remotePath](
                    RpcHelper::RpcOnce&& reply, auto&& channel) {
                    auto self = weak.lock();
                    if (!self)
                        return reply({{"error", "Session no longer exists"}});

                    const auto result = self->operationQueue_->addDownloadOperation(
                        *channel, Ids::makeOperationId(newOperationIdString), localPath, remotePath);

                    if (!result.has_value())
                    {
                        Log::error(
                            "Failed to add download operation for file '{}' to '{}': {}",
                            remotePath,
                            localPath,
                            result.error().toString());
                        return reply({{"error", result.error().toString()}});
                    }

                    Log::info(
                        "Added download operation with id '{}' for file '{}' to '{}'",
                        newOperationIdString,
                        remotePath,
                        localPath);

                    self->resetQueueThrottle();
                    reply({{"success", true}});
                },
                std::move(reply));
        });
}

void Session::registerRpcSftpAddUploadOperation()
{
    on(fmt::format("Session::{}::sftp::addUpload", id_.value()))
        .perform([weak = weak_from_this()](
                     RpcHelper::RpcOnce&& reply,
                     std::string const& channelIdString,
                     std::string const& newOperationIdString,
                     std::string const& localPath,
                     std::string const& remotePath) {
            auto self = weak.lock();
            if (!self)
                return reply({{"error", "Session no longer exists"}});

            self->withSftpChannelDo(
                Ids::makeChannelId(channelIdString),
                [weak = self->weak_from_this(), newOperationIdString, localPath, remotePath](
                    RpcHelper::RpcOnce&& reply, auto&& channel) {
                    auto self = weak.lock();
                    if (!self)
                        return reply({{"error", "Session no longer exists"}});

                    const auto result = self->operationQueue_->addUploadOperation(
                        *channel, Ids::makeOperationId(newOperationIdString), localPath, remotePath);

                    if (!result.has_value())
                    {
                        Log::error(
                            "Failed to add upload operation for file '{}' to '{}': {}",
                            localPath,
                            remotePath,
                            result.error().toString());
                        return reply({{"error", result.error().toString()}});
                    }

                    Log::info(
                        "Added upload operation with id '{}' for file '{}' to '{}'",
                        newOperationIdString,
                        localPath,
                        remotePath);

                    self->resetQueueThrottle();
                    reply({{"success", true}});
                },
                std::move(reply));
        });
}

void Session::registerOperationQueuePauseUnpause()
{
    on(fmt::format("OperationQueue::{}::pauseUnpause", id_.value()))
        .perform([weak = weak_from_this()](RpcHelper::RpcOnce&& reply, bool pause) {
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
        });
}