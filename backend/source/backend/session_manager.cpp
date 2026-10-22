#include <backend/session_manager.hpp>
#include <utility/error_message.hpp>
#include <backend/rpc_helper.hpp>
#include <log/log.hpp>

#include <nui/utility/scope_exit.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/dispatch.hpp>
#include <libssh/libssh.h>
#include <roar/utility/base64.hpp>
#include <roar/filesystem/special_paths.hpp>

#include <optional>
#include <thread>
#include <future>
#include <mutex>
#include <cstring>
#include <ranges>

using namespace Detail;

namespace
{
    using ProviderIterator = std::map<int, PasswordProvider*>::const_iterator;

    void askNextProvider(
        ProviderIterator iter,
        ProviderIterator end,
        std::weak_ptr<PasswordRequest> weakRequest,
        std::string const& whatFor,
        std::string const& prompt
    )
    {
        const auto request = weakRequest.lock();
        if (!request)
            return;

        if (iter == end)
            return request->resolve(std::nullopt);

        iter->second->getPassword(
            whatFor,
            prompt,
            [iter, end, weakRequest, whatFor, prompt](std::optional<std::string> password)
            {
                if (password.has_value())
                {
                    if (const auto pending = weakRequest.lock(); pending)
                        pending->resolve(std::move(password));
                    return;
                }
                askNextProvider(std::next(iter), end, weakRequest, whatFor, prompt);
            }
        );
    }
}

int askPassDefault(char const* prompt, char* buf, std::size_t length, int, int, void* userdata)
{
    std::pair<SessionManager*, std::string> const* data =
        static_cast<std::pair<SessionManager*, std::string>*>(userdata);

    auto* manager = data->first;
    std::string whatFor = data->second;

    if (!manager->strand_->running_in_this_thread())
    {
        throw std::runtime_error(
            "askPassDefault called outside of strand - This critical bug will crash the application."
        );
    }

    if (manager->passwordProviders_.empty())
        return -1;

    const auto request = manager->beginPasswordRequest();
    if (!request)
        return -1;
    const auto endRequest = Nui::ScopeExit(
        [manager]() noexcept
        {
            manager->endPasswordRequest();
        }
    );

    askNextProvider(manager->passwordProviders_.begin(), manager->passwordProviders_.end(), request, whatFor, prompt);

    const auto password = request->wait();

    if (password.has_value())
    {
        std::memset(buf, 0, length);
        std::strncpy(buf, password.value().c_str(), length - 1);
        return 0;
    }
    return -1;
}

SessionManager::SessionManager(
    boost::asio::any_io_executor executor,
    Persistence::StateHolder& stateHolder,
    Nui::Window& wnd,
    Nui::RpcHub& hub
)
    : RpcHelper::StrandRpc{executor, wnd, hub}
    , stateHolder_{&stateHolder}
    , localOperationQueue_{std::make_shared<LocalOperationQueue>(executor_, strand_, wnd, hub, localDeletionPrevented_)}
{}

void SessionManager::preventLocalDeletion(bool prevent)
{
    localDeletionPrevented_->store(prevent);
}

void SessionManager::startFileTracking(std::filesystem::path const& temporaryDownloadsDirectory)
{
    temporaryInstances_ =
        std::make_unique<FileTracking::TempDirInstanceManager>(*strand_, temporaryDownloadsDirectory);
    temporaryInstances_->scheduleStartupCleanup();
    localWatches_ = std::make_unique<FileTracking::LocalWatchManager>(*strand_, temporaryDownloadsDirectory);
    registerRpcOrphanedInstances();
}

void SessionManager::registerRpcOrphanedInstances()
{
    on("FileTracking::countOrphaned")
        .perform(
            [this](RpcHelper::RpcOnce&& reply)
            {
                reply({{"success", true}, {"count", temporaryInstances_->countOrphaned()}});
            }
        );
    on("FileTracking::forceCleanup")
        .perform(
            [this](RpcHelper::RpcOnce&& reply)
            {
                reply({{"success", true}, {"removed", temporaryInstances_->removeOrphaned()}});
            }
        );
}

void SessionManager::fileTrackingDefaults(Persistence::FileTrackingOptions const& options)
{
    within_strand_do(
        [this,
         defaults = SharedData::WatchOptions{
             .autoReupload = options.autoReupload,
             .moveRemoteOnLocalMove = options.moveRemoteOnLocalMove,
             .deleteRemoteOnLocalDelete = options.deleteRemoteOnLocalDelete,
             .checkRemoteChanges = options.checkRemoteChanges.value_or(true),
         }]()
        {
            fileTrackingDefaults_ = defaults;
        }
    );
}

SessionManager::~SessionManager()
{
    for (auto& [id, session] : sessions_)
        session->stop();
}

void SessionManager::shutdown()
{
    {
        std::scoped_lock lock{passwordRequestMutex_};
        passwordPromptsCancelled_ = true;
        if (pendingPasswordRequest_)
            pendingPasswordRequest_->resolve(std::nullopt);
    }

    if (strand_->running_in_this_thread())
    {
        Log::error("SessionManager::shutdown called from within the strand, not waiting for queued work.");
        return;
    }

    std::promise<void> strandDrained;
    within_strand_do_no_recurse(
        [&strandDrained]()
        {
            strandDrained.set_value();
        }
    );
    strandDrained.get_future().wait();
}

std::shared_ptr<PasswordRequest> SessionManager::beginPasswordRequest()
{
    std::scoped_lock lock{passwordRequestMutex_};
    if (passwordPromptsCancelled_)
        return nullptr;
    pendingPasswordRequest_ = std::make_shared<PasswordRequest>();
    return pendingPasswordRequest_;
}

void SessionManager::endPasswordRequest() noexcept
{
    std::scoped_lock lock{passwordRequestMutex_};
    pendingPasswordRequest_.reset();
}

void SessionManager::addPasswordProvider(int priority, PasswordProvider* provider)
{
    within_strand_do(
        [this, priority, provider]()
        {
            passwordProviders_.emplace(priority, provider);
        }
    );
}

void SessionManager::addSession(
    Persistence::SshSessionOptions const& sessionOptions,
    std::function<void(std::optional<Ids::SessionId> const&, std::optional<std::string> const& errorMessage)> onComplete
)
{
    within_strand_do(
        [this, sessionOptions, onComplete = std::move(onComplete)]()
        {
            const auto sessionId = Ids::SessionId{Ids::generateId()};

            std::pair<SessionManager*, std::string> askPassUserDataKeyPhrase{this, "keyPhrase"};
            std::pair<SessionManager*, std::string> askPassUserDataPassword{this, "password"};
            auto maybeSshSession = SecureShell::makeSession(
                sessionOptions,
                askPassDefault,
                &askPassUserDataKeyPhrase,
                &askPassUserDataPassword,
                nullptr,
                // Self destruct callback
                [weak = weak_from_this(), sessionId]()
                {
                    auto self = weak.lock();
                    if (!self)
                        return;
                    self->removeSession(sessionId, /*connectionLost=*/true);

                    // Notify frontend of session loss:
                    self->wnd_->runInJavascriptThread(
                        [hub_ = self->hub_, sessionId]()
                        {
                            try
                            {
                                RpcHelper::callRemoteSafely(
                                    *hub_, fmt::format("Session::{}::onDisconnect", sessionId.value())
                                );
                            }
                            catch (const std::exception& e)
                            {
                                // There should never anything throw here, but lets log just in case and dont let an
                                // exception escape out of this lambda.
                                Log::error(
                                    "Failed to call Session::{}::onDisconnect: {}.",
                                    sessionId.value(),
                                    Utility::errorMessage(e)
                                );
                            }
                        }
                    );
                }
            );

            if (maybeSshSession)
            {
                const auto session = std::make_shared<Session>(
                    sessionId,
                    std::move(maybeSshSession).value(),
                    executor_,
                    strand_,
                    *wnd_,
                    *hub_,
                    sessionOptions.sftpOptions.value(),
                    &bulkResumeRegistry_,
                    localOperationQueue_,
                    FileTracking::TrackerServices{
                        .instances = temporaryInstances_.get(),
                        .localWatches = localWatches_.get(),
                        .defaultOptions =
                            [this]()
                        {
                            return fileTrackingDefaults_;
                        },
                    }
                );
                const auto emplaced = sessions_.emplace(sessionId, session);
                if (!emplaced.second)
                {
                    Log::error("Session id collision - This should never happen.");
                    return onComplete(std::nullopt, "Session id collision");
                }

                Log::info("Created session with id '{}', total is now '{}'.", sessionId.value(), sessions_.size());
                session->start();
                onComplete(sessionId, std::nullopt);
            }
            else
            {
                Log::error("Failed to create session: {}", maybeSshSession.error());
                onComplete(std::nullopt, maybeSshSession.error());
            }
        }
    );
}

void SessionManager::removeSession(Ids::SessionId sessionId, bool connectionLost)
{
    within_strand_do(
        [this, sessionId, connectionLost]()
        {
            if (auto iter = sessions_.find(sessionId); iter != sessions_.end())
            {
                Log::info("Removing session with id: {}", sessionId.value());
                if (connectionLost)
                    iter->second->onConnectionLost();
                iter->second->stop();
                sessions_.erase(iter);
            }
            else
            {
                Log::warn("Cannot remove session, no session found with id: {}", sessionId.value());
            }
        }
    );
}

void SessionManager::registerRpc()
{
    registerRpcSessionConnect();
    registerRpcSessionDisconnect();
    registerRpcAdoptBulkResume();
    registerRpcDiscardBulkResume();
    registerRpcStopFileTracking();
    localOperationQueue_->registerRpc();
}

void SessionManager::registerRpcStopFileTracking()
{
    on("FileTracker::stopAll")
        .perform(
            [this](RpcHelper::RpcOnce&& reply)
            {
                for (auto const& session : std::views::values(sessions_))
                    session->stopFileTracking();
                reply({{"success", true}});
            }
        );
}

void SessionManager::registerRpcSessionConnect()
{
    on("SessionManager::connect")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                Log::info("Connecting to ssh server with parameters: {}", parameters.dump(4));

                if (!RpcHelper::ParameterVerifyView{reply, "SessionManager::connect", parameters}.hasValueDeep(
                        "engineOptions"
                    ))
                {
                    return;
                }

                auto onComplete = rpcSafe(
                    std::move(reply),
                    [](auto const& reply, auto const& maybeId, auto const& maybeErrorMessage)
                    {
                        if (!maybeId)
                        {
                            Log::error(
                                "Failed to connect to ssh server: {}", maybeErrorMessage.value_or("unknown error")
                            );
                            return reply(
                                {{"error",
                                    maybeErrorMessage.value_or("backend.sessionManager.connectFailedUnspecified")}}
                            );
                        }

                        Log::info("Connected to ssh server with id: {}", maybeId->value());
                        return reply({{"id", maybeId->value()}});
                    }
                );

                addSession(parameters["engineOptions"].get<Persistence::SshSessionOptions>(), std::move(onComplete));
            }
        );
}

void SessionManager::registerRpcSessionDisconnect()
{
    on("SessionManager::disconnect")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (!RpcHelper::ParameterVerifyView{reply, "SessionManager::disconnect", parameters}.hasValueDeep(
                        "sessionId"
                    ))
                    return;

                try
                {
                    removeSession(Ids::makeSessionId(parameters["sessionId"].get<std::string>()));
                    return reply({{"success", true}});
                }
                catch (std::exception const& e)
                {
                    Log::error("Error disconnecting to ssh server: {}", Utility::errorMessage(e));
                    return reply({{"error", Utility::errorMessage(e)}});
                }
            }
        );
}

void SessionManager::registerRpcAdoptBulkResume()
{
    on("SessionManager::adoptBulkResume")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (!RpcHelper::ParameterVerifyView{reply, "SessionManager::adoptBulkResume", parameters}.hasValueDeep(
                        "sessionId"
                    ))
                    return;
                if (!RpcHelper::ParameterVerifyView{reply, "SessionManager::adoptBulkResume", parameters}.hasValueDeep(
                        "operationIds"
                    ))
                    return;

                try
                {
                    const auto sessionId = Ids::makeSessionId(parameters["sessionId"].get<std::string>());
                    std::vector<Ids::OperationId> ids;
                    for (auto const& raw : parameters["operationIds"])
                        ids.push_back(Ids::makeOperationId(raw.get<std::string>()));

                    within_strand_do(
                        [weak = weak_from_this(), sessionId, ids = std::move(ids)]()
                        {
                            auto self = weak.lock();
                            if (!self)
                                return;
                            const auto iter = self->sessions_.find(sessionId);
                            if (iter == self->sessions_.end())
                            {
                                Log::warn(
                                    "adoptBulkResume: no session '{}' found — leaving registry untouched",
                                    sessionId.value()
                                );
                                return;
                            }
                            iter->second->adoptBulkResumes(ids);
                        }
                    );
                    return reply({{"success", true}});
                }
                catch (std::exception const& e)
                {
                    Log::error("Error in adoptBulkResume: {}", Utility::errorMessage(e));
                    return reply({{"error", Utility::errorMessage(e)}});
                }
            }
        );
}

void SessionManager::registerRpcDiscardBulkResume()
{
    on("SessionManager::discardBulkResume")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (!RpcHelper::ParameterVerifyView{reply, "SessionManager::discardBulkResume", parameters}
                         .hasValueDeep("operationIds"))
                    return;

                try
                {
                    for (auto const& raw : parameters["operationIds"])
                        bulkResumeRegistry_.discard(Ids::makeOperationId(raw.get<std::string>()));
                    return reply({{"success", true}});
                }
                catch (std::exception const& e)
                {
                    Log::error("Error in discardBulkResume: {}", Utility::errorMessage(e));
                    return reply({{"error", Utility::errorMessage(e)}});
                }
            }
        );
}