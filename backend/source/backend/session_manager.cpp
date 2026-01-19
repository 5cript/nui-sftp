#include <backend/session_manager.hpp>
#include <log/log.hpp>

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

using namespace Detail;

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

    std::promise<std::optional<std::string>> pwPromise{};
    std::function<void(decltype(manager->passwordProviders_)::const_iterator)> askNextProvider;

    askNextProvider = [end = manager->passwordProviders_.end(), &askNextProvider, prompt, &pwPromise, &whatFor](
                          decltype(manager->passwordProviders_)::const_iterator iter
                      )
    {
        if (iter == end)
        {
            pwPromise.set_value(std::nullopt);
            return;
        }

        iter->second->getPassword(
            whatFor,
            prompt,
            [iter, &pwPromise, &askNextProvider](std::optional<std::string> pw) mutable
            {
                if (pw.has_value())
                    pwPromise.set_value(pw);
                else
                {
                    ++iter;
                    askNextProvider(iter);
                }
            }
        );
    };

    if (manager->passwordProviders_.empty())
    {
        return -1;
    }
    askNextProvider(manager->passwordProviders_.begin());

    const auto pw = pwPromise.get_future().get();

    if (pw.has_value())
    {
        std::memset(buf, 0, length);
        std::strncpy(buf, pw.value().c_str(), length - 1);
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
{}

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
    std::function<void(std::optional<Ids::SessionId> const&)> onComplete
)
{
    within_strand_do(
        [this, sessionOptions, onComplete = std::move(onComplete)]()
        {
            std::pair<SessionManager*, std::string> askPassUserDataKeyPhrase{this, "Key phrase"};
            std::pair<SessionManager*, std::string> askPassUserDataPassword{this, "Password"};
            auto maybeSshSession = makeSession(
                sessionOptions, askPassDefault, &askPassUserDataKeyPhrase, &askPassUserDataPassword, &pwCache_
            );

            if (maybeSshSession)
            {
                const auto sessionId = Ids::SessionId{Ids::generateId()};
                const auto session = std::make_shared<Session>(
                    sessionId,
                    std::move(maybeSshSession).value(),
                    executor_,
                    strand_,
                    *wnd_,
                    *hub_,
                    sessionOptions.sftpOptions.value()
                );
                const auto emplaced = sessions_.emplace(sessionId, session);
                if (!emplaced.second)
                {
                    Log::error("Session id collision - This should never happen.");
                    return onComplete(std::nullopt);
                }

                Log::info("Created session with id '{}', total is now '{}'.", sessionId.value(), sessions_.size());
                session->start();
                onComplete(sessionId);
            }
            else
            {
                Log::error("Failed to create session: {}", maybeSshSession.error());
                onComplete(std::nullopt);
            }
        }
    );
}

void SessionManager::removeSession(Ids::SessionId sessionId)
{
    within_strand_do(
        [this, sessionId]()
        {
            if (auto iter = sessions_.find(sessionId); iter != sessions_.end())
            {
                Log::info("Removing session with id: {}", sessionId.value());
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
                    [](auto const& reply, auto const& maybeId)
                    {
                        if (!maybeId)
                        {
                            Log::error("Failed to connect to ssh server");
                            return reply({{"error", "Failed to connect to ssh server"}});
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
                    Log::error("Error disconnecting to ssh server: {}", e.what());
                    return reply({{"error", e.what()}});
                }
            }
        );
}