#include <backend/ftp/ftp_connect.hpp>
#include <ftp/client/data_connection.hpp>
#include <log/log.hpp>
#include <remote_io/remote_error.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <stop_token>
#include <string_view>
#include <utility>

namespace FtpConnect
{
    namespace
    {
        using FileTransferProtocol::FtpConnectionOptions;
        using RemoteIo::RemoteErrorKind;

        constexpr std::string_view anonymousUser = "anonymous";
        /** @brief What an anonymous login sends as its password, a placeholder mail address as curl and wget send. */
        constexpr std::string_view anonymousPassword = "anonymous@";

        using Applied = std::expected<void, std::string>;

        /**
         * @brief Sets @p target to @p seconds when they are set.
         *
         * @return Fails for less than one second.
         */
        template <typename DurationT>
        Applied applySeconds(std::optional<int> const& seconds, std::string_view name, DurationT& target)
        {
            if (!seconds)
                return {};
            if (*seconds < 1)
                return std::unexpected(
                    fmt::format("The FTP option {} must be at least 1 second, it is {}.", name, *seconds)
                );
            target = std::chrono::seconds{*seconds};
            return {};
        }

        FileTransferProtocol::TlsMode tlsModeOf(Persistence::FtpTlsMode mode)
        {
            switch (mode)
            {
                case Persistence::FtpTlsMode::Plain:
                    return FileTransferProtocol::TlsMode::None;
                case Persistence::FtpTlsMode::Implicit:
                    return FileTransferProtocol::TlsMode::Implicit;
                case Persistence::FtpTlsMode::Explicit:
                    break;
            }
            return FileTransferProtocol::TlsMode::Explicit;
        }

        FileTransferProtocol::ExtendedPassive extendedPassiveOf(Persistence::FtpExtendedPassive choice)
        {
            switch (choice)
            {
                case Persistence::FtpExtendedPassive::Always:
                    return FileTransferProtocol::ExtendedPassive::Always;
                case Persistence::FtpExtendedPassive::Never:
                    return FileTransferProtocol::ExtendedPassive::Never;
                case Persistence::FtpExtendedPassive::Automatic:
                    break;
            }
            return FileTransferProtocol::ExtendedPassive::Automatic;
        }

        Applied applyServer(Persistence::FtpSessionOptions const& session, FileTransferProtocol::PoolOptions& pool)
        {
            if (session.host.empty())
                return std::unexpected("The FTP session names no server.");
            const auto port = session.effectivePort();
            if (port < 1 || port > std::numeric_limits<std::uint16_t>::max())
                return std::unexpected(fmt::format("The port {} of the FTP session is not a valid port.", port));
            pool.host = session.host;
            pool.port = static_cast<std::uint16_t>(port);
            return {};
        }

        std::expected<std::optional<FileTransferProtocol::Sha256Digest>, std::string>
        pinOf(std::optional<std::string> const& pinnedPublicKeySha256)
        {
            if (!pinnedPublicKeySha256)
                return std::nullopt;
            const auto digest = FileTransferProtocol::parseDigest(*pinnedPublicKeySha256);
            if (!digest)
                return std::unexpected("The pinned public key of the FTP server is not a SHA-256 digest.");
            return digest;
        }

        Applied applyTls(
            Persistence::FtpSessionOptions const& session,
            std::shared_ptr<FileTransferProtocol::TlsContext> tlsContext,
            FileTransferProtocol::LoginOptions& login
        )
        {
            login.tlsMode = tlsModeOf(session.tlsMode);
            if (login.tlsMode == FileTransferProtocol::TlsMode::None)
                return {};
            if (!tlsContext)
                return std::unexpected("The FTP session asks for TLS, but no TLS context was made for it.");
            return pinOf(session.pinnedPublicKeySha256)
                .transform(
                    [&session, &tlsContext, &login](std::optional<FileTransferProtocol::Sha256Digest> pin)
                    {
                        login.tls = FileTransferProtocol::TlsConnectionSettings{
                            .context = std::move(tlsContext),
                            .serverName = session.host,
                            .pinnedPublicKey = pin,
                        };
                    }
                );
        }

        Applied applyTimeouts(Persistence::FtpOptions const& ftp, FtpConnectionOptions& options)
        {
            auto& transfer = options.fileSystem.transfer;
            return applySeconds(ftp.connectTimeoutSeconds, "connectTimeoutSeconds", options.pool.control.connectTimeout)
                .and_then(
                    [&ftp, &transfer]()
                    {
                        return applySeconds(
                            ftp.connectTimeoutSeconds, "connectTimeoutSeconds", transfer.data.connectTimeout
                        );
                    }
                )
                .and_then(
                    [&ftp, &options]()
                    {
                        return applySeconds(
                            ftp.commandTimeoutSeconds, "commandTimeoutSeconds", options.pool.control.commandTimeout
                        );
                    }
                )
                .and_then(
                    [&ftp, &transfer]()
                    {
                        return applySeconds(
                            ftp.dataInactivityTimeoutSeconds,
                            "dataInactivityTimeoutSeconds",
                            transfer.data.inactivityTimeout
                        );
                    }
                )
                .and_then(
                    [&ftp, &transfer]()
                    {
                        return applySeconds(
                            ftp.transferReplyTimeoutSeconds, "transferReplyTimeoutSeconds", transfer.finalReplyTimeout
                        );
                    }
                );
        }

        Applied applyKeepalive(Persistence::FtpOptions const& ftp, FileTransferProtocol::PoolOptions& pool)
        {
            auto& keepalive = pool.control.socket.keepalive;
            if (ftp.keepIdleAlive)
                pool.keepIdleAlive = *ftp.keepIdleAlive;
            return applySeconds(ftp.idleNoopIntervalSeconds, "idleNoopIntervalSeconds", pool.idleNoopInterval)
                .and_then(
                    [&ftp, &keepalive]()
                    {
                        return applySeconds(ftp.keepaliveIdleSeconds, "keepaliveIdleSeconds", keepalive.idle);
                    }
                )
                .and_then(
                    [&ftp, &keepalive]()
                    {
                        return applySeconds(
                            ftp.keepaliveIntervalSeconds, "keepaliveIntervalSeconds", keepalive.interval
                        );
                    }
                )
                .and_then(
                    [&ftp, &keepalive]() -> Applied
                    {
                        if (!ftp.keepaliveCount)
                            return {};
                        if (*ftp.keepaliveCount < 1)
                            return std::unexpected(
                                fmt::format(
                                    "The FTP option keepaliveCount must be at least 1, it is {}.", *ftp.keepaliveCount
                                )
                            );
                        keepalive.count = *ftp.keepaliveCount;
                        return {};
                    }
                );
        }

        /**
         * @brief min(concurrency, maxConnections - 1) transfer connections beside C0, the default of the client for
         *        maxConnections when it is unset.
         */
        Applied applyTransferConnections(
            Persistence::FtpOptions const& ftp,
            Persistence::SftpOptions const& transfers,
            FileTransferProtocol::PoolOptions& pool
        )
        {
            const auto defaultMaxConnections =
                static_cast<int>(FileTransferProtocol::PoolOptions{}.transferConnections) + 1;
            const auto maxConnections = ftp.maxConnections.value_or(defaultMaxConnections);
            if (maxConnections < 2)
                return std::unexpected(
                    fmt::format("The FTP option maxConnections must be at least 2, it is {}.", maxConnections)
                );
            const auto concurrency = transfers.concurrency.value_or(Persistence::defaultSftpConcurrency);
            if (concurrency < 1)
                return std::unexpected(
                    fmt::format("The transfer option concurrency must be at least 1, it is {}.", concurrency)
                );
            pool.transferConnections = static_cast<std::size_t>(std::min(concurrency, maxConnections - 1));
            return {};
        }

        void applyPassiveMode(Persistence::FtpOptions const& ftp, FileTransferProtocol::DataConnectionOptions& data)
        {
            if (ftp.extendedPassive)
                data.extendedPassive = extendedPassiveOf(*ftp.extendedPassive);
            if (ftp.trustPassiveAddress)
                data.trustPassiveAddress = *ftp.trustPassiveAddress;
        }

        std::chrono::milliseconds
        excessOver(std::chrono::milliseconds configured, std::chrono::milliseconds clientDefault)
        {
            return std::max(configured - clientDefault, std::chrono::milliseconds{0});
        }

        /**
         * @brief The commands a listing waits for in turn: CWD, EPSV, PASV after a refused EPSV and LIST.
         */
        constexpr int commandsOfAnOperation = 4;

        /**
         * @brief The commands the stream call that opens a transfer waits for in turn: SIZE, EPSV, PASV after a refused
         *        EPSV, REST and RETR or STOR.
         */
        constexpr int commandsOfATransferCall = 5;

        /**
         * @brief Moves each backstop back by the raises over their defaults of the timeouts its steps run into one
         *        after another. An operation: the wait for C0 while it logs in again, the commands of a listing, its
         *        data connection, its data and the final reply. A stream call: the commands that open a transfer, its
         *        data connection, its data and the final reply.
         */
        void keepBackstopsBehindTimeouts(FtpConnectionOptions& options)
        {
            const FtpConnectionOptions defaults{};
            auto const& data = options.fileSystem.transfer.data;
            const auto controlConnect =
                excessOver(options.pool.control.connectTimeout, defaults.pool.control.connectTimeout);
            const auto command = excessOver(options.pool.control.commandTimeout, defaults.pool.control.commandTimeout);
            const auto dataConnect = excessOver(data.connectTimeout, defaults.fileSystem.transfer.data.connectTimeout);
            const auto dataInactivity =
                excessOver(data.inactivityTimeout, defaults.fileSystem.transfer.data.inactivityTimeout);
            const auto finalReply = excessOver(
                options.fileSystem.transfer.finalReplyTimeout, defaults.fileSystem.transfer.finalReplyTimeout
            );
            const auto transferSteps = dataConnect + dataInactivity + finalReply;
            options.fileSystem.operationLimits.backstop +=
                controlConnect + commandsOfAnOperation * command + transferSteps;
            options.fileSystem.transferCallLimits.backstop += commandsOfATransferCall * command + transferSteps;
        }

        std::expected<std::shared_ptr<FileTransferProtocol::TlsContext>, std::string>
        tlsContextOf(Persistence::FtpSessionOptions const& session)
        {
            if (session.tlsMode == Persistence::FtpTlsMode::Plain)
                return nullptr;
            return FileTransferProtocol::TlsContext::create(tlsContextOptionsOf(session.ftpOptions.value()))
                .transform_error(
                    [](RemoteIo::RemoteError const& error)
                    {
                        return fmt::format("TLS could not be prepared for the FTP session: {}", error.toString());
                    }
                );
        }

        /**
         * @brief @p options with the password @p askPassword gives, unless the login is anonymous.
         */
        std::expected<FtpConnectionOptions, std::string> withPassword(
            FtpConnectionOptions options,
            Persistence::FtpSessionOptions const& session,
            PasswordSource const& askPassword
        )
        {
            if (isAnonymous(session.loginUser()))
                return options;
            auto& credentials = options.pool.login.credentials;
            auto password = askPassword(passwordPrompt(credentials.user, session.host));
            if (!password)
                return std::unexpected("The password prompt was cancelled.");
            credentials.password = *std::move(password);
            return options;
        }

        /**
         * @brief @p sentence, with ": " and @p reason when there is one, else with a full stop.
         */
        std::string withReason(std::string sentence, std::string const& reason)
        {
            if (reason.empty())
                return fmt::format("{}.", sentence);
            return fmt::format("{}: {}", sentence, reason);
        }

        std::string untrustedCertificateMessage(
            std::optional<FileTransferProtocol::CertificateInfo> const& certificate,
            std::string const& server
        )
        {
            if (!certificate)
                return fmt::format("The certificate of the FTP server {} is not trusted.", server);
            const auto reasons = FileTransferProtocol::reasonsOf(certificate->verifyFailures);
            const auto why = reasons.empty() ? std::string{} : fmt::format(" ({})", reasons);
            return fmt::format(
                "The certificate of the FTP server {} is not trusted{}. Its SHA-256 fingerprint is {}.",
                server,
                why,
                FileTransferProtocol::formatDigest(certificate->certificateSha256)
            );
        }

        std::string noSecureConnectionMessage(
            Persistence::FtpSessionOptions const& session,
            std::string const& server,
            std::string const& reason
        )
        {
            if (session.tlsMode == Persistence::FtpTlsMode::Plain)
                return withReason(
                    fmt::format(
                        "The FTP server {} requires TLS, choose explicit or implicit TLS for this session", server
                    ),
                    reason
                );
            return withReason(fmt::format("TLS could not be set up with the FTP server {}", server), reason);
        }

        std::string stoppedMessage(Persistence::FtpSessionOptions const& session)
        {
            return fmt::format(
                "The connect to the FTP server {}:{} was stopped.", session.host, session.effectivePort()
            );
        }

        using LoginOutcome = std::expected<FileTransferProtocol::LoginResult, FileTransferProtocol::LoginFailure>;

        /**
         * @brief How often the wait for a login looks whether the connect is to stop: the result of the login comes as
         *        a future, which nothing else can wake.
         */
        constexpr auto stopCheckInterval = std::chrono::milliseconds{50};

        /**
         * @brief Waits for @p outcome until it completes or @p stop is requested.
         *
         * @return Nothing after a stop request.
         */
        template <typename OutcomeT>
        std::optional<OutcomeT> awaitOutcome(std::future<OutcomeT>& outcome, std::stop_token const& stop)
        {
            while (outcome.wait_for(stopCheckInterval) == std::future_status::timeout)
            {
                if (stop.stop_requested())
                    return std::nullopt;
            }
            return outcome.get();
        }

        /**
         * @brief Whether the server of @p options answers and its certificate is trusted, see reachServer().
         *
         * @return Why it is not reachable, Cancelled after a stop request.
         */
        std::expected<void, FileTransferProtocol::LoginFailure>
        reach(FtpEngineContext& engine, FtpConnectionOptions const& options, std::stop_token const& stop)
        {
            auto reached = FileTransferProtocol::reachServer(engine.executor(), engine.resolver(), options.pool, stop);
            auto outcome = awaitOutcome(reached, stop);
            // A stop ends the reach too, which may complete as cancelled before the wait sees the stop.
            if (!outcome || stop.stop_requested())
                return std::unexpected(
                    FileTransferProtocol::LoginFailure{.error = {.kind = RemoteErrorKind::Cancelled}}
                );
            return outcome->transform([](auto const&) {});
        }

        /**
         * @brief @p failure in words, the stopped message after a stop request.
         */
        std::string reachFailureMessage(
            FileTransferProtocol::LoginFailure const& failure,
            Persistence::FtpSessionOptions const& session,
            std::stop_token const& stop
        )
        {
            if (stop.stop_requested())
                return stoppedMessage(session);
            return loginFailureMessage(failure, session);
        }

        /**
         * @brief @p options with the key of the certificate that @p failure refused pinned, when @p askTrust trusts
         *        it.
         */
        std::expected<FtpConnectionOptions, std::string> trustedByTheUser(
            FtpConnectionOptions options,
            FileTransferProtocol::LoginFailure const& failure,
            Persistence::FtpSessionOptions const& session,
            TrustSource const& askTrust,
            std::stop_token const& stop
        )
        {
            auto& tls = options.pool.login.tls;
            if (failure.error.kind != RemoteErrorKind::TrustRequired || !failure.untrustedCertificate || !tls)
                return std::unexpected(reachFailureMessage(failure, session, stop));
            auto const& certificate = *failure.untrustedCertificate;
            const auto trustedKey = askTrust(
                TrustQuestion{
                    .host = session.host,
                    .port = options.pool.port,
                    .certificate = certificate,
                    // A key that matches the pin is trusted, so a pin here is another one.
                    .pinnedKeyDiffers = tls->pinnedPublicKey.has_value(),
                }
            );
            if (stop.stop_requested())
                return std::unexpected(stoppedMessage(session));
            if (!trustedKey)
                return std::unexpected(
                    fmt::format(
                        "The certificate of the FTP server {}:{} was not trusted.", session.host, options.pool.port
                    )
                );
            if (*trustedKey != certificate.publicKeySha256)
                return std::unexpected(
                    fmt::format(
                        "The key trusted for the FTP server {}:{} is not the key of its certificate.",
                        session.host,
                        options.pool.port
                    )
                );
            Log::info(
                "FTP: The user trusts the certificate of {}:{} for this connect.", session.host, options.pool.port
            );
            tls->pinnedPublicKey = *trustedKey;
            return options;
        }

        /**
         * @brief @p options once the server answered and its certificate is trusted, so nobody is asked for the
         *        password of a server that is not there or not trusted. A certificate nobody vouched for is put to
         *        @p askTrust, and the server is reached again with the key the user trusted, which is not asked about
         *        twice. An anonymous login without TLS asks nothing, its login reaches the server.
         */
        std::expected<FtpConnectionOptions, std::string> reachedFirst(
            FtpEngineContext& engine,
            FtpConnectionOptions options,
            Persistence::FtpSessionOptions const& session,
            TrustSource const& askTrust,
            std::stop_token const& stop
        )
        {
            if (isAnonymous(session.loginUser()) && options.pool.login.tlsMode == FileTransferProtocol::TlsMode::None)
                return options;
            const auto reached = reach(engine, options, stop);
            if (reached)
                return options;
            return trustedByTheUser(std::move(options), reached.error(), session, askTrust, stop)
                .and_then(
                    [&engine,
                        &session,
                        &stop](FtpConnectionOptions pinned) -> std::expected<FtpConnectionOptions, std::string>
                    {
                        const auto reachedAgain = reach(engine, pinned, stop);
                        if (!reachedAgain)
                            return std::unexpected(reachFailureMessage(reachedAgain.error(), session, stop));
                        return pinned;
                    }
                );
        }

        std::expected<SessionConnection, std::string> logIn(
            FtpEngineContext& engine,
            FtpConnectionOptions options,
            Persistence::FtpSessionOptions const& session,
            std::function<void()> onConnectionLoss,
            std::stop_token const& stop
        )
        {
            if (stop.stop_requested())
                return std::unexpected(stoppedMessage(session));
            auto connection = std::make_unique<FileTransferProtocol::FtpConnection>(
                engine.executor(),
                engine.resolver(),
                std::move(options),
                [onConnectionLoss = std::move(onConnectionLoss)](RemoteIo::RemoteError const& error)
                {
                    Log::warn("FTP: The session was lost: {}", error.toString());
                    if (onConnectionLoss)
                        onConnectionLoss();
                }
            );
            auto login = connection->logIn();
            const auto outcome = awaitOutcome(login, stop);
            // The destructor of the connection fails a login that still runs.
            if (!outcome)
                return std::unexpected(stoppedMessage(session));
            auto const& loggedIn = *outcome;
            if (!loggedIn)
                return std::unexpected(loginFailureMessage(loggedIn.error(), session));
            Log::info("FTP: Logged in to {}:{}.", session.host, session.effectivePort());
            return SessionConnection{.connection = std::move(connection), .terminalHost = nullptr};
        }
    }

    bool isAnonymous(Persistence::FtpUser const& user)
    {
        if (!user.name || user.name->empty())
            return true;
        const auto name = Utility::Algorithm::toLowerCase(*user.name);
        return name == anonymousUser || name == "ftp";
    }

    std::string passwordPrompt(std::string const& user, std::string const& host)
    {
        return fmt::format("Password for {}@{}:", user, host);
    }

    FileTransferProtocol::Credentials credentialsOf(Persistence::FtpUser const& user)
    {
        FileTransferProtocol::Credentials credentials{};
        if (user.account && !user.account->empty())
            credentials.account = user.account;
        if (user.name && !user.name->empty())
            credentials.user = *user.name;
        else
            credentials.user = anonymousUser;
        if (isAnonymous(user))
            credentials.password = anonymousPassword;
        return credentials;
    }

    FileTransferProtocol::TlsContextOptions tlsContextOptionsOf(Persistence::FtpOptions const& options)
    {
        FileTransferProtocol::TlsContextOptions contextOptions{};
        contextOptions.systemAuthorities = true;
        if (options.minimumTlsVersion)
            contextOptions.minimumVersion = *options.minimumTlsVersion == Persistence::FtpTlsVersion::Tls13
                ? FileTransferProtocol::TlsVersion::Tls13
                : FileTransferProtocol::TlsVersion::Tls12;
        return contextOptions;
    }

    std::expected<FtpConnectionOptions, std::string> connectionOptionsOf(
        Persistence::FtpSessionOptions const& session,
        std::shared_ptr<FileTransferProtocol::TlsContext> tlsContext
    )
    {
        FtpConnectionOptions options{};
        options.pool.login.credentials = credentialsOf(session.loginUser());
        auto const& ftp = session.ftpOptions.value();
        return applyServer(session, options.pool)
            .and_then(
                [&session, &tlsContext, &options]()
                {
                    return applyTls(session, std::move(tlsContext), options.pool.login);
                }
            )
            .and_then(
                [&ftp, &options]()
                {
                    return applyTimeouts(ftp, options);
                }
            )
            .and_then(
                [&ftp, &options]()
                {
                    return applyKeepalive(ftp, options.pool);
                }
            )
            .and_then(
                [&ftp, &session, &options]()
                {
                    return applyTransferConnections(ftp, session.sftpOptions.value(), options.pool);
                }
            )
            .transform(
                [&ftp, &options]()
                {
                    applyPassiveMode(ftp, options.fileSystem.transfer.data);
                    keepBackstopsBehindTimeouts(options);
                    return std::move(options);
                }
            );
    }

    std::string loginFailureMessage(
        FileTransferProtocol::LoginFailure const& failure,
        Persistence::FtpSessionOptions const& session
    )
    {
        auto const& error = failure.error;
        const auto server = fmt::format("{}:{}", session.host, session.effectivePort());
        switch (error.kind)
        {
            case RemoteErrorKind::TrustRequired:
                return untrustedCertificateMessage(failure.untrustedCertificate, server);
            case RemoteErrorKind::NoSecureConnection:
                return noSecureConnectionMessage(session, server, error.message);
            case RemoteErrorKind::NoConnection:
                return withReason(fmt::format("The FTP server {} could not be reached", server), error.message);
            case RemoteErrorKind::TimedOut:
                return withReason(fmt::format("The FTP server {} did not answer in time", server), error.message);
            case RemoteErrorKind::AuthenticationFailed:
                return withReason(
                    fmt::format(
                        "The FTP server {} refused the login of {}", server, credentialsOf(session.loginUser()).user
                    ),
                    error.message
                );
            case RemoteErrorKind::ConnectionLimit:
                return withReason(fmt::format("The FTP server {} refused another connection", server), error.message);
            case RemoteErrorKind::ConnectionLost:
                return withReason(
                    fmt::format("The FTP server {} ended the connection during the login", server), error.message
                );
            case RemoteErrorKind::Cancelled:
                return fmt::format("The login to the FTP server {} was cancelled.", server);
            default:
                return fmt::format("The login to the FTP server {} failed: {}", server, error.toString());
        }
    }

    std::expected<SessionConnection, std::string> connect(
        FtpEngineContext& engine,
        Persistence::FtpSessionOptions const& session,
        PasswordSource const& askPassword,
        TrustSource const& askTrust,
        std::function<void()> onConnectionLoss,
        std::stop_token const& stop
    )
    {
        if (stop.stop_requested())
            return std::unexpected(stoppedMessage(session));
        return tlsContextOf(session)
            .and_then(
                [&session](std::shared_ptr<FileTransferProtocol::TlsContext> tlsContext)
                {
                    return connectionOptionsOf(session, std::move(tlsContext));
                }
            )
            .and_then(
                [&engine, &session, &askTrust, &stop](FtpConnectionOptions options)
                {
                    return reachedFirst(engine, std::move(options), session, askTrust, stop);
                }
            )
            .and_then(
                [&session, &askPassword](FtpConnectionOptions options)
                {
                    return withPassword(std::move(options), session, askPassword);
                }
            )
            .and_then(
                [&engine, &session, &onConnectionLoss, &stop](FtpConnectionOptions options)
                {
                    return logIn(engine, std::move(options), session, std::move(onConnectionLoss), stop);
                }
            );
    }
}
