#include <backend/session_manager.hpp>
#include <backend/ftp/ftp_connect.hpp>
#include <utility/algorithm/case_convert.hpp>
#include <utility/error_message.hpp>
#include <backend/rpc_helper.hpp>
#include <log/log.hpp>

#include <nui/utility/scope_exit.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <roar/utility/base64.hpp>
#include <roar/filesystem/special_paths.hpp>
#include <fmt/format.h>

#include <cassert>
#include <optional>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <cstring>
#include <ranges>
#include <string_view>

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

    /**
     * @brief Whether the errors of a connect that fails are shown as toasts, a reconnect attempt that is retried
     *        asks for none.
     */
    Log::Toast failureToastOf(nlohmann::json const& parameters)
    {
        const auto found = parameters.find("showFailureToast");
        if (found != parameters.end() && found->is_boolean() && !found->get<bool>())
            return Log::Toast::Skip;
        return Log::Toast::Show;
    }

    /**
     * @brief The engineOptions of a connect as @p OptionsT.
     */
    template <typename OptionsT>
    std::expected<OptionsT, std::string> engineOptionsOf(nlohmann::json const& parameters)
    {
        const auto found = parameters.find("engineOptions");
        if (found == parameters.end())
            return std::unexpected("A connect needs engineOptions.");
        if (!found->is_object())
            return std::unexpected("The engineOptions of a connect are no object.");
        try
        {
            return found->get<OptionsT>();
        }
        catch (std::exception const& exception)
        {
            return std::unexpected(
                fmt::format("The engineOptions of a connect cannot be read: {}", Utility::errorMessage(exception))
            );
        }
    }

    /**
     * @brief The name of a connected engine in log messages.
     */
    std::string_view engineNameOf(Persistence::TerminalEngineType type)
    {
        if (type == Persistence::TerminalEngineType::ftp)
            return "FTP";
        return "SSH";
    }
}

std::expected<Persistence::TerminalEngineType, std::string> connectedEngineOf(nlohmann::json const& parameters)
{
    const auto found = parameters.find("engineType");
    if (found == parameters.end())
        return Persistence::TerminalEngineType::ssh;
    if (!found->is_string())
        return std::unexpected("The engineType of a connect is not a string.");
    const auto name = found->get<std::string>();
    const auto type = Persistence::terminalEngineTypeFromName(name);
    if (!type || *type == Persistence::TerminalEngineType::shell)
        return std::unexpected(fmt::format("Cannot connect to a server with the engine '{}'.", name));
    return *type;
}

int askPassDefault(char const* prompt, char* buf, std::size_t length, int, int, void* userdata) noexcept
{
    std::pair<SessionManager*, std::string> const* data =
        static_cast<std::pair<SessionManager*, std::string>*>(userdata);

    auto* manager = data->first;
    assert(manager->strand_->running_in_this_thread());

    try
    {
        const auto password = manager->askPassword(data->second, prompt);
        if (!password)
            return -1;
        std::memset(buf, 0, length);
        std::strncpy(buf, password->c_str(), length - 1);
        return 0;
    }
    catch (...)
    {
        // Nothing may unwind through the C frames of libssh.
        return -1;
    }
}

SessionManager::SessionManager(
    boost::asio::any_io_executor executor,
    Persistence::StateHolder& stateHolder,
    Nui::Window& wnd,
    Nui::RpcHub& hub,
    FtpEngineContext& ftpEngine
)
    : RpcHelper::StrandRpc{executor, wnd, hub}
    , stateHolder_{&stateHolder}
    , ftpEngine_{&ftpEngine}
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
    connectsStop_.request_stop();
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
        [this, &strandDrained]()
        {
            const auto drained = Nui::ScopeExit(
                [&strandDrained]() noexcept
                {
                    strandDrained.set_value();
                }
            );
            closeAllSessions();
        }
    );
    strandDrained.get_future().wait();
}

void SessionManager::closeAllSessions()
{
    sessionsClosed_ = true;
    if (!sessions_.empty())
        Log::info("Closing all {} session(s).", sessions_.size());
    for (auto const& session : std::views::values(sessions_))
        session->stop();
    sessions_.clear();
}

std::optional<std::string> SessionManager::askProviders(std::string const& whatFor, std::string const& prompt)
{
    if (passwordProviders_.empty())
    {
        Log::warn("There is no password provider to ask for the {}.", whatFor);
        return std::nullopt;
    }

    const auto request = beginPasswordRequest();
    if (!request)
        return std::nullopt;
    const auto endRequest = Nui::ScopeExit(
        [this]() noexcept
        {
            endPasswordRequest();
        }
    );

    askNextProvider(passwordProviders_.begin(), passwordProviders_.end(), request, whatFor, prompt);
    return request->wait();
}

std::optional<std::string> SessionManager::askPassword(std::string const& whatFor, std::string const& prompt) noexcept
{
    // A prompt that fails is no declined one, its error is reported.
    lastPromptDeclined_ = false;
    try
    {
        auto password = askProviders(whatFor, prompt);
        lastPromptDeclined_ = !password;
        return password;
    }
    catch (std::exception const& exception)
    {
        Log::error("Asking for a password failed: {}", Utility::errorMessage(exception));
    }
    catch (...)
    {
        Log::error("Asking for a password failed.");
    }
    return std::nullopt;
}

template <typename Answer>
Answer SessionManager::awaitPromptAnswer(Answer refused, std::function<void(std::function<void(Answer)>)> const& ask)
{
    const auto request = beginPasswordRequest();
    if (!request)
        return refused;
    const auto endRequest = Nui::ScopeExit(
        [this]() noexcept
        {
            endPasswordRequest();
        }
    );

    // The request only wakes the wait and lets shutdown() cancel it, the answer travels beside it.
    const auto given = std::make_shared<std::atomic<Answer>>(refused);
    ask(
        [request, given](Answer answer)
        {
            given->store(answer);
            request->resolve(std::string{});
        }
    );
    if (!request->wait())
        return refused;
    return given->load();
}

FtpConnect::TrustAnswer SessionManager::askTrustProvider(FtpConnect::TrustQuestion const& question)
{
    if (!trustProvider_)
    {
        Log::warn("There is no trust provider to ask about the certificate of {}:{}.", question.host, question.port);
        return FtpConnect::TrustAnswer::Refused;
    }

    return awaitPromptAnswer<FtpConnect::TrustAnswer>(
        FtpConnect::TrustAnswer::Refused,
        [this, &question](std::function<void(FtpConnect::TrustAnswer)> onAnswer)
        {
            trustProvider_->askTrust(question, std::move(onAnswer));
        }
    );
}

std::optional<SecureShell::HostKeyAnswer>
SessionManager::askHostKeyProvider(SecureShell::HostKeyQuestion const& question)
{
    // A changed key is only shown, nothing that was trusted before speaks for it.
    const auto unknown = question.problem == SecureShell::HostKeyProblem::Unknown;
    // The known hosts files compare hosts without regard to case.
    const auto server = std::pair{Utility::Algorithm::toLowerCase(question.host), question.port};
    const auto presentedKey = fmt::format("{} {}", question.keyType, question.fingerprint);
    const auto trusted = hostKeysOfThisRun_.find(server);
    const auto trustedInThisRun = unknown && trusted != hostKeysOfThisRun_.end();
    // The known hosts file did not take the key or no longer holds it, and is left as it is.
    if (trustedInThisRun && trusted->second == presentedKey)
        return SecureShell::HostKeyAnswer::Once;

    if (!hostKeyProvider_)
    {
        Log::warn("There is no host key provider to ask about the key of {}:{}.", question.host, question.port);
        return std::nullopt;
    }
    const auto prompt = [this](SecureShell::HostKeyQuestion const& shown)
    {
        return awaitPromptAnswer<SecureShell::HostKeyAnswer>(
            SecureShell::HostKeyAnswer::Refused,
            [this, &shown](std::function<void(SecureShell::HostKeyAnswer)> onAnswer)
            {
                hostKeyProvider_->askAboutHostKey(shown, std::move(onAnswer));
            }
        );
    };

    // The known hosts files cannot tell that the key changed, the key trusted for the server in this run can.
    if (trustedInThisRun)
    {
        Log::warn(
            "The host key of {}:{} is not the one trusted for it in this run, the connection is refused: {}",
            question.host,
            question.port,
            presentedKey
        );
        auto changed = question;
        changed.problem = SecureShell::HostKeyProblem::ChangedSinceTrusted;
        changed.trustedKey = trusted->second;
        prompt(changed);
        return SecureShell::HostKeyAnswer::Refused;
    }

    const auto answer = prompt(question);
    // The known hosts file may fail to take the key.
    if (unknown && answer != SecureShell::HostKeyAnswer::Refused)
        hostKeysOfThisRun_[server] = presentedKey;
    return answer;
}

SecureShell::HostKeyAnswer SessionManager::askAboutHostKey(SecureShell::HostKeyQuestion const& question) noexcept
{
    // A prompt that fails is no declined one, its error is reported.
    lastPromptDeclined_ = false;
    try
    {
        const auto answer = askHostKeyProvider(question);
        // Nobody saw a prompt or a warning, so the error of the connect is the report.
        if (!answer)
            return SecureShell::HostKeyAnswer::Refused;
        // A changed key is refused whatever the answer, and its warning is the report of that.
        lastPromptDeclined_ = *answer == SecureShell::HostKeyAnswer::Refused ||
            question.problem != SecureShell::HostKeyProblem::Unknown;
        return *answer;
    }
    catch (std::exception const& exception)
    {
        Log::error("Asking whether to trust a host key failed: {}", Utility::errorMessage(exception));
    }
    catch (...)
    {
        Log::error("Asking whether to trust a host key failed.");
    }
    return SecureShell::HostKeyAnswer::Refused;
}

FtpConnect::TrustAnswer SessionManager::askTrust(FtpConnect::TrustQuestion const& question) noexcept
{
    // A prompt that fails is no declined one, its error is reported.
    lastPromptDeclined_ = false;
    try
    {
        const auto answer = askTrustProvider(question);
        lastPromptDeclined_ = answer == FtpConnect::TrustAnswer::Refused;
        return answer;
    }
    catch (std::exception const& exception)
    {
        Log::error("Asking whether to trust a certificate failed: {}", Utility::errorMessage(exception));
    }
    catch (...)
    {
        Log::error("Asking whether to trust a certificate failed.");
    }
    return FtpConnect::TrustAnswer::Refused;
}

SecureShell::HostKeySource SessionManager::hostKeySource()
{
    return [this](SecureShell::HostKeyQuestion const& question)
    {
        return askAboutHostKey(question);
    };
}

FtpConnect::TrustSource SessionManager::trustSource()
{
    return [this](FtpConnect::TrustQuestion const& question)
    {
        return askTrust(question);
    };
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

void SessionManager::setTrustProvider(FtpConnect::TrustProvider* provider)
{
    within_strand_do(
        [this, provider]()
        {
            trustProvider_ = provider;
        }
    );
}

void SessionManager::setHostKeyProvider(HostKeyProvider* provider)
{
    within_strand_do(
        [this, provider]()
        {
            hostKeyProvider_ = provider;
        }
    );
}

void SessionManager::setTrustStore(std::shared_ptr<FtpConnect::CertificateTrustStore> store)
{
    within_strand_do(
        [this, store = std::move(store)]() mutable
        {
            trustStore_ = std::move(store);
        }
    );
}

SessionManager::Connect SessionManager::secureShellConnect(Persistence::SshSessionOptions sessionOptions)
{
    return [this, sessionOptions = std::move(sessionOptions)](
               std::function<void()> onConnectionLoss
           ) -> std::expected<SessionConnection, std::string>
    {
        std::pair<SessionManager*, std::string> askPassUserDataKeyPhrase{this, "keyPhrase"};
        std::pair<SessionManager*, std::string> askPassUserDataPassword{this, "password"};
        return SecureShell::makeSession(
            sessionOptions,
            askPassDefault,
            &askPassUserDataKeyPhrase,
            &askPassUserDataPassword,
            nullptr,
            hostKeySource(),
            std::move(onConnectionLoss)
        )
            .transform(&SessionConnection::fromSecureShell);
    };
}

SessionManager::Connect
SessionManager::noticingDeclinedPasswords(Connect connect, std::shared_ptr<bool> passwordDeclined)
{
    return [this, connect = std::move(connect), passwordDeclined = std::move(passwordDeclined)](
               std::function<void()> onConnectionLoss
           ) -> std::expected<SessionConnection, std::string>
    {
        lastPromptDeclined_ = false;
        auto connection = connect(std::move(onConnectionLoss));
        *passwordDeclined = lastPromptDeclined_;
        return connection;
    };
}

SessionManager::Connect SessionManager::ftpConnect(Persistence::FtpSessionOptions sessionOptions)
{
    return [this, sessionOptions = std::move(sessionOptions)](
               std::function<void()> onConnectionLoss
           ) -> std::expected<SessionConnection, std::string>
    {
        return FtpConnect::connect(
            *ftpEngine_,
            sessionOptions,
            [this](std::string const& prompt)
            {
                return askPassword("password", prompt);
            },
            *trustStore_,
            trustSource(),
            [](std::vector<FileTransferProtocol::DerCertificate> const& chain,
                std::string const& serverName,
                std::stop_token const& stop)
            {
                return FileTransferProtocol::platformVerdictOf(chain, serverName, stop);
            },
            std::move(onConnectionLoss),
            connectsStop_.get_token()
        );
    };
}

void SessionManager::addSession(
    Connect connect,
    Persistence::SftpOptions sftpOptions,
    std::function<void(std::optional<Ids::SessionId> const&, std::optional<std::string> const& errorMessage)> onComplete
)
{
    within_strand_do(
        [this, connect = std::move(connect), sftpOptions = std::move(sftpOptions), onComplete = std::move(onComplete)]()
        {
            if (sessionsClosed_)
                return onComplete(std::nullopt, "The application is closing, no session is connected any more.");

            const auto sessionId = Ids::SessionId{Ids::generateId()};

            auto connection = connect(
                // Self destruct callback, posted: a connection may report its loss from within its start() on the
                // strand.
                [weak = weak_from_this(), strand = strand_, sessionId]()
                {
                    boost::asio::post(
                        *strand,
                        [weak, sessionId]()
                        {
                            if (auto self = weak.lock(); self)
                                self->onSessionLost(sessionId);
                        }
                    );
                }
            );

            if (connection)
            {
                const auto session = std::make_shared<Session>(
                    sessionId,
                    *std::move(connection),
                    executor_,
                    strand_,
                    *wnd_,
                    *hub_,
                    sftpOptions,
                    &bulkResumeRegistry_,
                    keptTransferRegistry_,
                    localOperationQueue_,
                    FileTracking::TrackerServices{
                        .instances = temporaryInstances_.get(),
                        .localWatches = localWatches_.get(),
                        .defaultOptions = [this]()
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
                onComplete(std::nullopt, connection.error());
            }
        }
    );
}

void SessionManager::removeSession(Ids::SessionId sessionId)
{
    within_strand_do(
        [this, sessionId]()
        {
            eraseSession(sessionId, /*connectionLost=*/false);
        }
    );
}

bool SessionManager::eraseSession(Ids::SessionId const& sessionId, bool connectionLost)
{
    const auto iter = sessions_.find(sessionId);
    if (iter == sessions_.end())
    {
        Log::warn("Cannot remove session, no session found with id: {}", sessionId.value());
        return false;
    }
    Log::info("Removing session with id: {}", sessionId.value());
    if (connectionLost)
        iter->second->onConnectionLost();
    iter->second->stop();
    sessions_.erase(iter);
    return true;
}

void SessionManager::onSessionLost(Ids::SessionId const& sessionId)
{
    if (!eraseSession(sessionId, /*connectionLost=*/true))
        return;

    wnd_->runInJavascriptThread(
        [hub = hub_, sessionId]()
        {
            try
            {
                RpcHelper::callRemoteSafely(*hub, fmt::format("Session::{}::onDisconnect", sessionId.value()));
            }
            catch (const std::exception& e)
            {
                // There should never anything throw here, but lets log just in case and dont let an
                // exception escape out of this lambda.
                Log::error(
                    "Failed to call Session::{}::onDisconnect: {}.", sessionId.value(), Utility::errorMessage(e)
                );
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
    registerRpcForgetKeptTransfers();
    registerRpcDiscardKeptTransfers();
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

std::expected<SessionManager::ConnectRequest, std::string>
SessionManager::connectRequestOf(nlohmann::json const& parameters, std::shared_ptr<bool> passwordDeclined)
{
    return connectedEngineOf(parameters)
        .and_then(
            [this,
                &parameters,
                &passwordDeclined](Persistence::TerminalEngineType engine) -> std::expected<ConnectRequest, std::string>
            {
                if (engine == Persistence::TerminalEngineType::ftp)
                {
                    return engineOptionsOf<Persistence::FtpSessionOptions>(parameters)
                        .transform(
                            [this, &passwordDeclined](Persistence::FtpSessionOptions sessionOptions)
                            {
                                // Not logged as a whole.
                                Log::info(
                                    "Connecting to FTP server {}:{}",
                                    sessionOptions.host,
                                    sessionOptions.effectivePort()
                                );
                                auto sftpOptions = sessionOptions.sftpOptions.value();
                                return ConnectRequest{
                                    .engine = Persistence::TerminalEngineType::ftp,
                                    .connect = noticingDeclinedPasswords(
                                        ftpConnect(std::move(sessionOptions)), std::move(passwordDeclined)
                                    ),
                                    .sftpOptions = std::move(sftpOptions),
                                };
                            }
                        );
                }
                Log::info("Connecting to ssh server with parameters: {}", parameters.dump(4));
                return engineOptionsOf<Persistence::SshSessionOptions>(parameters)
                    .transform(
                        [this, &passwordDeclined](Persistence::SshSessionOptions sessionOptions)
                        {
                            auto sftpOptions = sessionOptions.sftpOptions.value();
                            return ConnectRequest{
                                .engine = Persistence::TerminalEngineType::ssh,
                                .connect = noticingDeclinedPasswords(
                                    secureShellConnect(std::move(sessionOptions)), std::move(passwordDeclined)
                                ),
                                .sftpOptions = std::move(sftpOptions),
                            };
                        }
                    );
            }
        );
}

void SessionManager::registerRpcSessionConnect()
{
    on("SessionManager::connect")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                auto passwordDeclined = std::make_shared<bool>(false);
                auto request = connectRequestOf(parameters, passwordDeclined);
                if (!request)
                {
                    Log::error("Cannot connect: {}", request.error());
                    return reply.error(request.error());
                }

                const auto failureToast = failureToastOf(parameters);
                auto onComplete = rpcSafe(
                    std::move(reply),
                    [failureToast,
                        engineName = engineNameOf(request->engine),
                        passwordDeclined](auto const& reply, auto const& maybeId, auto const& maybeErrorMessage)
                    {
                        if (!maybeId)
                        {
                            // The user cancelled the password prompt, which is no failure to report.
                            if (*passwordDeclined)
                                Log::debug("The login to the {} server ended without a password", engineName);
                            else
                                Log::error(
                                    failureToast,
                                    "Failed to connect to {} server: {}",
                                    engineName,
                                    maybeErrorMessage.value_or("unknown error")
                                );
                            return reply(
                                {{"error",
                                     maybeErrorMessage.value_or("backend.sessionManager.connectFailedUnspecified")},
                                    {"cancelled", *passwordDeclined}}
                            );
                        }

                        Log::info("Connected to {} server with id: {}", engineName, maybeId->value());
                        return reply({{"id", maybeId->value()}});
                    }
                );

                addSession(std::move(request->connect), std::move(request->sftpOptions), std::move(onComplete));
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

void SessionManager::registerRpcForgetKeptTransfers()
{
    on("SessionManager::forgetKeptTransfers")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (!RpcHelper::ParameterVerifyView{reply, "SessionManager::forgetKeptTransfers", parameters}
                        .hasValueDeep("operationIds"))
                    return;

                try
                {
                    for (auto const& raw : parameters["operationIds"])
                        std::ignore = keptTransferRegistry_->take(Ids::makeOperationId(raw.get<std::string>()));
                    return reply({{"success", true}});
                }
                catch (std::exception const& e)
                {
                    Log::error("Error in forgetKeptTransfers: {}", Utility::errorMessage(e));
                    return reply({{"error", Utility::errorMessage(e)}});
                }
            }
        );
}

void SessionManager::registerRpcDiscardKeptTransfers()
{
    on("SessionManager::discardKeptTransfers")
        .perform(
            [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
            {
                if (!RpcHelper::ParameterVerifyView{reply, "SessionManager::discardKeptTransfers", parameters}
                         .hasValueDeep("operationIds"))
                    return;

                try
                {
                    auto discarded = nlohmann::json::array();
                    for (auto const& raw : parameters["operationIds"])
                    {
                        auto operationId = raw.get<std::string>();
                        if (keptTransferRegistry_->discardWithoutSession(Ids::makeOperationId(operationId)))
                            discarded.push_back(std::move(operationId));
                    }
                    return reply({{"success", true}, {"discarded", std::move(discarded)}});
                }
                catch (std::exception const& error)
                {
                    Log::error("Error in discardKeptTransfers: {}", Utility::errorMessage(error));
                    return reply({{"error", Utility::errorMessage(error)}});
                }
            }
        );
}
