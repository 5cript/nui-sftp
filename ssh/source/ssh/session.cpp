#include <ssh/session.hpp>
#include <ssh/sequential.hpp>
#include <ssh/sftp_session.hpp>
#include <ssh/u8_path.hpp>
#include <ssh/ssh_key.hpp>

#include <fmt/format.h>
#include <libssh/sftp.h>

namespace SecureShell
{
    namespace
    {
        void removeFromContainer(auto& container, auto* ptr, bool isBackElement)
        {
            if (isBackElement && container.back().get() == ptr)
            {
                container.pop_back();
            }
            else
            {
                auto it = std::find_if(
                    container.begin(),
                    container.end(),
                    [ptr](const auto& c)
                    {
                        return c.get() == ptr;
                    }
                );
                if (it != container.end())
                    container.erase(it);
            }
        }
    }

    Session::Session()
        : processingThread_{}
        , session_{}
        , channels_{}
    {}

    void Session::start()
    {
        processingThread_.start(std::chrono::milliseconds{1});
    }

    void Session::stop()
    {
        processingThread_.stop();
    }

    bool Session::isRunning() const
    {
        return processingThread_.isRunning();
    }

    void Session::shutdown()
    {
        removeAllChannels();
        removeAllSftpSessions();
        processingThread_.stop();
        session_.disconnect();
    }

    Session::~Session()
    {
        shutdown();
    }

    void Session::removeAllChannels()
    {
        while (!channels_.empty())
        {
            channels_.back()->close(true);
        }
    }

    void Session::removeAllSftpSessions()
    {
        while (!sftpSessions_.empty())
        {
            sftpSessions_.back()->close(false);
        }
    }

    std::string Session::getErrorMessage()
    {
        return fmt::format("SSH Error (code {}): {}", session_.getErrorCode(), session_.getError());
    }

    void Session::channelRemoveItself(Channel* channel, bool isBackElement)
    {
        removeFromContainer(channels_, channel, isBackElement);
    }
    void Session::sftpSessionRemoveItself(SftpSession* sftpSession, bool isBackElement)
    {
        removeFromContainer(sftpSessions_, sftpSession, isBackElement);
    }

    std::future<std::expected<std::weak_ptr<Channel>, int>> Session::createPtyChannel(PtyCreationOptions options)
    {
        auto promise = std::make_shared<std::promise<std::expected<std::weak_ptr<Channel>, int>>>();

        processingThread_.pushTask(
            [this, options = std::move(options), promise]() mutable
            {
                auto ptyChannel = std::make_unique<ssh::Channel>(session_);
                auto& channel = *ptyChannel;
                auto result = Detail::sequential(
                    [&channel]()
                    {
                        if (!channel.isOpen())
                            return channel.openSession();
                        return 0;
                    },
                    [&channel, &environment = options.environment]()
                    {
                        if (!environment.has_value())
                            return 0;
                        for (auto const& [key, value] : *environment)
                        {
                            if (channel.requestEnv(key.c_str(), value.c_str()) != 0)
                                return -1;
                        }
                        return 0;
                    },
                    [&channel, &options]()
                    {
                        return channel.requestPty(options.terminalType.c_str(), options.columns, options.rows);
                    },
                    [&channel, &options]()
                    {
                        if (!options.requestShell)
                            return 0;
                        return channel.requestShell();
                    }
                );

                if (result.result != SSH_OK)
                {
                    return promise->set_value(std::unexpected(session_.getErrorCode()));
                }

                auto sharedChannel =
                    std::make_shared<Channel>(this, processingThread_.createStrand(), std::move(ptyChannel));
                channels_.push_back(sharedChannel);
                return promise->set_value(sharedChannel);
            }
        );

        return promise->get_future();
    }

    std::future<std::expected<std::weak_ptr<SftpSession>, SftpError>> Session::createSftpSession()
    {
        auto promise = std::make_shared<std::promise<std::expected<std::weak_ptr<SftpSession>, SftpError>>>();

        processingThread_.pushTask(
            [this, promise]() -> void
            {
                auto sftp = sftp_new(session_.getCSession());
                if (sftp == nullptr)
                {
                    return promise->set_value(
                        std::unexpected(
                            SftpError{
                                .message = session_.getError(),
                                .sshError = session_.getErrorCode(),
                                .sftpError = 0,
                            }
                        )
                    );
                }

                auto result = sftp_init(sftp);
                if (result != SSH_OK)
                {
                    promise->set_value(
                        std::unexpected(
                            SftpError{
                                .message = session_.getError(),
                                .sshError = session_.getErrorCode(),
                                .sftpError = sftp_get_error(sftp),
                            }
                        )
                    );
                    sftp_free(sftp);
                }

                auto sftpSession = std::make_shared<SftpSession>(this, processingThread_.createStrand(), sftp);
                promise->set_value(sftpSession);
                sftpSessions_.push_back(sftpSession);
            }
        );

        return promise->get_future();
    }

    std::expected<std::unique_ptr<Session>, std::string> makeSession(
        Persistence::SshSessionOptions const& sessionOptions,
        AskPassCallback askPass,
        void* askPassUserDataKeyPhrase,
        void* askPassUserDataPassword,
        std::vector<PasswordCacheEntry>* pwCache
    )
    {
        auto session = std::make_unique<Session>();

        const auto sshOptions = sessionOptions.sshOptions.value();

#ifndef _WIN32
        // TODO: Implement this for Windows (its complicated, because there is no one single agent)
        const bool tryAgent =
            sshOptions.tryAgentForAuthentication ? sshOptions.tryAgentForAuthentication.value() : false;
#endif

        std::optional<std::string> lastAttemptedAppliedOption = "";

        auto result = Detail::sequential(
            [&]
            {
                lastAttemptedAppliedOption = "Log Verbosity";
                if (sshOptions.logVerbosity)
                {
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_LOG_VERBOSITY, static_cast<int>(sshOptions.logVerbosity.value())
                    );
                }
                return 0;
            },
            [&]
            {
                lastAttemptedAppliedOption = "Port";
                int port = 22;
                if (sessionOptions.port)
                    port = sessionOptions.port.value();
                return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_PORT, port);
            },
            [&]
            {
                lastAttemptedAppliedOption = "Host";
                return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_HOST, sessionOptions.host.c_str());
            },
            [&]
            {
                if (sessionOptions.user.has_value())
                {
                    lastAttemptedAppliedOption = "User";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_USER, sessionOptions.user.value().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.sshDirectory.has_value())
                {
                    lastAttemptedAppliedOption = "SSH Directory";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_SSH_DIR, sshOptions.sshDirectory.value().generic_string().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.identities.has_value())
                {
                    lastAttemptedAppliedOption = "Identities";
                    for (const auto& identity : sshOptions.identities.value())
                    {
                        auto res =
                            static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_ADD_IDENTITY, identity.c_str());
                        if (res != 0)
                            return res;
                    }
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.knownHostsFile.has_value())
                {
                    lastAttemptedAppliedOption = "Known Hosts File";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_KNOWNHOSTS, sshOptions.knownHostsFile.value().generic_string().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.connectTimeoutSeconds.has_value())
                {
                    lastAttemptedAppliedOption = "Connect Timeout Seconds";
                    long timeout = sshOptions.connectTimeoutSeconds.value();
                    return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_TIMEOUT, &timeout);
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.connectTimeoutUSeconds.has_value())
                {
                    lastAttemptedAppliedOption = "Connect Timeout Microseconds";
                    long timeout = sshOptions.connectTimeoutUSeconds.value();
                    return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_TIMEOUT_USEC, &timeout);
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.keyExchangeAlgorithms.has_value())
                {
                    lastAttemptedAppliedOption = "Key Exchange Algorithms";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_KEY_EXCHANGE, sshOptions.keyExchangeAlgorithms.value().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.compressionClientToServer.has_value())
                {
                    lastAttemptedAppliedOption = "Compression Client to Server";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_COMPRESSION_C_S, sshOptions.compressionClientToServer.value() ? "yes" : "no"
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.compressionServerToClient.has_value())
                {
                    lastAttemptedAppliedOption = "Compression Server to Client";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_COMPRESSION_S_C, sshOptions.compressionServerToClient.value() ? "yes" : "no"
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.compressionLevel.has_value())
                {
                    lastAttemptedAppliedOption = "Compression Level";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_COMPRESSION_LEVEL, sshOptions.compressionLevel.value()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.strictHostKeyCheck)
                {
                    lastAttemptedAppliedOption = "Strict Host Key Check";
                    int strict = sshOptions.strictHostKeyCheck.value() ? 1 : 0;
                    return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.proxyCommand)
                {
                    lastAttemptedAppliedOption = "Proxy Command";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_PROXYCOMMAND, sshOptions.proxyCommand.value().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.proxyJump)
                {
                    lastAttemptedAppliedOption = "Proxy Jump";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_PROXYJUMP, sshOptions.proxyJump.value().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.gssapiServerIdentity)
                {
                    lastAttemptedAppliedOption = "GSSAPI Server Identity";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_GSSAPI_SERVER_IDENTITY, sshOptions.gssapiServerIdentity.value().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.gssapiDelegateCredentials)
                {
                    lastAttemptedAppliedOption = "GSSAPI Delegate Credentials";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_GSSAPI_DELEGATE_CREDENTIALS, sshOptions.gssapiDelegateCredentials.value() ? 1 : 0
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.gssapiClientIdentity)
                {
                    lastAttemptedAppliedOption = "GSSAPI Client Identity";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_GSSAPI_CLIENT_IDENTITY, sshOptions.gssapiClientIdentity.value().c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.noDelay)
                {
                    lastAttemptedAppliedOption = "No Delay";
                    int noDelay = sshOptions.noDelay.value() ? 1 : 0;
                    return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_NODELAY, &noDelay);
                }
                return 0;
            },
            [&]
            {
                // I dont quite understand this option. "use pubkey auth in auto pubkey auth"???
                // Also the KBDINT auth and GSSAPI auth options for ssh_userauth_publickey_auto arent implemented.
                if (sessionOptions.sshOptions->usePublicKeyAutoAuth && *sessionOptions.sshOptions->usePublicKeyAutoAuth)
                {
                    lastAttemptedAppliedOption = "Use Public Key Auto Auth";
                    int pubkeyAuth = 1;
                    return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_PUBKEY_AUTH, &pubkeyAuth);
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.bypassConfig)
                {
                    lastAttemptedAppliedOption = "Bypass Config";
                    int processConfig = sshOptions.bypassConfig.value() ? 0 : 1;
                    return static_cast<ssh::Session&>(*session).setOption(SSH_OPTIONS_PROCESS_CONFIG, &processConfig);
                }
                return 0;
            },
            [&]
            {
                if (sshOptions.identityAgent)
                {
                    lastAttemptedAppliedOption = "Identity Agent";
                    return static_cast<ssh::Session&>(*session).setOption(
                        SSH_OPTIONS_IDENTITY_AGENT, u8Path(sshOptions.identityAgent.value()).c_str()
                    );
                }
                return 0;
            },
            [&]
            {
                // clear last attempted option, option setting succeeded, so remove it for the error message
                lastAttemptedAppliedOption = std::nullopt;
                return static_cast<ssh::Session&>(*session).connect();
            },
#ifdef _WIN32
            []()
#else
            [tryAgent, &session]()
#endif
            {
#ifdef _WIN32
                return static_cast<int>(SSH_AUTH_AGAIN);
#else
                if (!tryAgent)
                    return static_cast<int>(SSH_AUTH_AGAIN);

                const auto result = ssh_userauth_agent(static_cast<ssh::Session&>(*session).getCSession(), nullptr);
                switch (result)
                {
                    case SSH_AUTH_SUCCESS:
                        return static_cast<int>(SSH_AUTH_SUCCESS);
                    case SSH_AUTH_DENIED:
                        Log::error("Authentication denied");
                        return static_cast<int>(SSH_AUTH_DENIED);
                    case SSH_AUTH_ERROR:
                        Log::error("Authentication error");
                        return static_cast<int>(SSH_AUTH_ERROR);
                    case SSH_AUTH_PARTIAL:
                        Log::error("Partial authentication");
                        return static_cast<int>(SSH_AUTH_PARTIAL);
                    case SSH_AUTH_AGAIN:
                        Log::error("Authentication again");
                        return static_cast<int>(SSH_AUTH_AGAIN);
                    default:
                        Log::error("Unknown authentication result");
                        return result;
                }
#endif
            }
        );

        if (result.result == SSH_ERROR)
        {
            return std::unexpected(
                fmt::format(
                    "Failed to connect: {}\nLast attempted option to set: {}",
                    session->getErrorMessage(),
                    lastAttemptedAppliedOption.value_or("None, all options applied successfully")
                )
            );
        }

        if (result.result != SSH_AUTH_SUCCESS && sshOptions.usePublicKeyAutoAuth &&
            sshOptions.usePublicKeyAutoAuth.value())
        {
            result = Detail::sequential(
                [&]
                {
                    return static_cast<ssh::Session&>(*session).userauthPublickeyAuto();
                }
            );
        }

        if (result.result != SSH_AUTH_SUCCESS && sessionOptions.sshKeyPrivate)
        {
            const auto sshKeyPrivate = sessionOptions.sshKeyPrivate.value();
            const auto maybeSshKeyPublic = sessionOptions.sshKeyPublic;

            SshKey publicKey{};
            SshKey privateKey{};
            result = Detail::sequential(
                [maybeSshKeyPublic, &publicKey]()
                {
                    if (maybeSshKeyPublic)
                    {
                        return ssh_pki_import_pubkey_file(
                            u8Path(*maybeSshKeyPublic).c_str(), &publicKey.underlyingReferenced()
                        );
                    }
                    return SSH_OK;
                },
                [&publicKey, &session]()
                {
                    if (publicKey)
                    {
                        return ssh_userauth_try_publickey(
                            static_cast<ssh::Session&>(*session).getCSession(),
                            nullptr, // Username. SHOULD BE NULL, most servers dont allow different usernames.
                            publicKey.underlying()
                        );
                    }
                    return SSH_OK;
                },
                [&sshKeyPrivate, &privateKey, askPass, askPassUserDataKeyPhrase]()
                {
                    return ssh_pki_import_privkey_file(
                        u8Path(sshKeyPrivate).c_str(),
                        nullptr,
                        askPass,
                        askPassUserDataKeyPhrase,
                        &privateKey.underlyingReferenced()
                    );
                },
                [&privateKey, &session]()
                {
                    return static_cast<ssh::Session&>(*session).userauthPublickey(privateKey.underlying());
                }
            );
        }

        const auto usePasswordAuth = !sshOptions.usePasswordAuth || sshOptions.usePasswordAuth.value();
        if (result.result != SSH_AUTH_SUCCESS && usePasswordAuth)
        {
            std::string buf(1024, '\0');
            result = Detail::sequential(
                [&]
                {
                    if (pwCache)
                    {
                        std::optional<std::string> pwFromCache{};
                        for (const auto& cache : *pwCache)
                        {
                            if (cache.user == sessionOptions.user && cache.host == sessionOptions.host &&
                                cache.port == sessionOptions.port)
                            {
                                pwFromCache = cache.password;
                                break;
                            }
                        }

                        if (pwFromCache.has_value())
                        {
                            buf = pwFromCache.value();
                            const auto result = static_cast<ssh::Session&>(*session).userauthPassword(buf.data());
                            if (result == SSH_AUTH_SUCCESS)
                                return (int)SSH_AUTH_SUCCESS;
                        }
                    }

                    const auto r = askPass("Password: ", buf.data(), buf.size(), 0, 0, askPassUserDataPassword);
                    if (r == 0)
                    {
                        const auto result = static_cast<ssh::Session&>(*session).userauthPassword(buf.data());
                        if (result == SSH_AUTH_SUCCESS)
                        {
                            if (pwCache)
                                pwCache->emplace_back(
                                    sessionOptions.user, sessionOptions.host, sessionOptions.port, buf
                                );
                            return (int)SSH_AUTH_SUCCESS;
                        }
                    }
                    return (int)SSH_AUTH_DENIED;
                }
            );
        }

        if (result.result != SSH_AUTH_SUCCESS)
        {
            std::string authResult = "";
            switch (result.result)
            {
                case SSH_AUTH_DENIED:
                    authResult = "Authentication denied";
                    break;
                case SSH_AUTH_ERROR:
                    authResult = "Authentication error";
                    break;
                case SSH_AUTH_PARTIAL:
                    authResult = "Partial authentication";
                    break;
                case SSH_AUTH_AGAIN:
                    authResult = "Authentication again";
                    break;
                default:
                    authResult = "Unknown authentication result";
                    break;
            }
            return std::unexpected(fmt::format("Failed to authenticate: {}", authResult));
        }

        return session;
    }
}