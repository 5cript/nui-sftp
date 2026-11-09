#include <ftp/client/login.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/reply_parsers.hpp>

#include <boost/asio/this_coro.hpp>

#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using RemoteIo::RemoteErrorKind;
        using Result = std::expected<void, RemoteIo::RemoteError>;

        RemoteIo::RemoteError errorOf(RemoteErrorKind kind, std::string message)
        {
            return RemoteIo::RemoteError{
                .kind = kind,
                .protocol = RemoteIo::Protocol::Ftp,
                .message = std::move(message),
            };
        }

        bool succeeded(Reply const& reply)
        {
            return classifyReply(reply.code) == ReplyClass::PositiveCompletion;
        }

        /**
         * @brief Runs @p command. A cancel that came with the reply counts too, the next co_await would throw.
         */
        asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> ask(ControlConnection& connection, Command command)
        {
            auto reply = co_await connection.execute(std::move(command));
            const auto state = co_await asio::this_coro::cancellation_state;
            if (state.cancelled() != asio::cancellation_type::none)
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The login was cancelled"));
            co_return reply;
        }

        /**
         * @brief @p command, whose reply has to be a 2xx.
         */
        asio::awaitable<Result> require(ControlConnection& connection, Command command)
        {
            const auto verb = command.verb;
            const auto reply = co_await ask(connection, std::move(command));
            if (!reply)
                co_return std::unexpected(reply.error());
            if (!succeeded(*reply))
                co_return std::unexpected(replyError(*reply, verb));
            co_return Result{};
        }

        /**
         * @brief ACCT after a 332, which says the login needs one.
         */
        asio::awaitable<Result> sendAccount(ControlConnection& connection, Credentials const& credentials)
        {
            if (!credentials.account)
                co_return std::unexpected(
                    errorOf(RemoteErrorKind::AuthenticationFailed, "The server asks for an account, none is set")
                );
            co_return co_await require(connection, Command{.verb = Verb::Account, .argument = *credentials.account});
        }

        /**
         * @brief USER, then PASS on a 331 and ACCT on a 332 (RFC 959 5.4). A 230 to USER needs no password.
         */
        asio::awaitable<Result> authenticate(ControlConnection& connection, Credentials const& credentials)
        {
            const auto user = co_await ask(connection, Command{.verb = Verb::User, .argument = credentials.user});
            if (!user)
                co_return std::unexpected(user.error());
            if (succeeded(*user))
                co_return Result{};
            if (user->code == 332)
                co_return co_await sendAccount(connection, credentials);
            if (user->code != 331)
                co_return std::unexpected(replyError(*user, Verb::User));

            const auto password =
                co_await ask(connection, Command{.verb = Verb::Password, .argument = credentials.password});
            if (!password)
                co_return std::unexpected(password.error());
            if (succeeded(*password))
                co_return Result{};
            if (password->code == 332)
                co_return co_await sendAccount(connection, credentials);
            co_return std::unexpected(replyError(*password, Verb::Password));
        }

        /**
         * @brief PBSZ 0 and PROT P, the data connections of a TLS session use TLS too. A refusal leaves no secure
         *        session.
         */
        asio::awaitable<Result> protectData(ControlConnection& connection)
        {
            const auto bufferSize =
                co_await require(connection, Command{.verb = Verb::ProtectionBufferSize, .argument = "0"});
            if (!bufferSize)
                co_return std::unexpected(asNoSecureConnection(bufferSize.error()));
            const auto protection =
                co_await require(connection, Command{.verb = Verb::DataProtection, .argument = "P"});
            if (!protection)
                co_return std::unexpected(asNoSecureConnection(protection.error()));
            co_return Result{};
        }

        asio::awaitable<std::expected<Features, RemoteIo::RemoteError>> askFeatures(ControlConnection& connection)
        {
            const auto reply = co_await ask(connection, Command{.verb = Verb::Features});
            if (!reply)
                co_return std::unexpected(reply.error());
            co_return Features::fromReply(*reply);
        }

        /**
         * @brief OPTS UTF8 ON, a refusal is no failure.
         */
        asio::awaitable<std::expected<bool, RemoteIo::RemoteError>> askForUtf8(ControlConnection& connection)
        {
            const auto reply = co_await ask(connection, Command{.verb = Verb::Options, .argument = "UTF8 ON"});
            if (!reply)
                co_return std::unexpected(reply.error());
            co_return succeeded(*reply);
        }

        asio::awaitable<std::expected<std::string, RemoteIo::RemoteError>> askHome(ControlConnection& connection)
        {
            const auto reply = co_await ask(connection, Command{.verb = Verb::PrintWorkingDirectory});
            if (!reply)
                co_return std::unexpected(reply.error());
            co_return parsePathReply(*reply, Verb::PrintWorkingDirectory);
        }

        asio::awaitable<std::expected<LoginResult, RemoteIo::RemoteError>> establish(
            std::shared_ptr<ControlConnection> const& connection,
            std::vector<asio::ip::tcp::endpoint> endpoints,
            LoginOptions const& options
        )
        {
            const bool secured = options.tlsMode != TlsMode::None;
            if (secured && !options.tls)
                co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "TLS was asked for without its settings"));

            LoginResult result{};
            auto implicitTls = options.tlsMode == TlsMode::Implicit ? options.tls : std::nullopt;
            auto greeting = co_await connection->connect(std::move(endpoints), std::move(implicitTls));
            if (!greeting)
                co_return std::unexpected(greeting.error());
            result.greeting = std::move(*greeting);

            if (options.tlsMode == TlsMode::Explicit)
            {
                if (const auto trusted = co_await connection->startTls(*options.tls); !trusted)
                    co_return std::unexpected(trusted.error());
            }
            result.certificate = connection->trustedCertificate();

            if (const auto authenticated = co_await authenticate(*connection, options.credentials); !authenticated)
                co_return std::unexpected(authenticated.error());

            if (secured)
            {
                if (const auto protectedData = co_await protectData(*connection); !protectedData)
                    co_return std::unexpected(protectedData.error());
                result.dataProtected = true;
            }

            auto features = co_await askFeatures(*connection);
            if (!features)
                co_return std::unexpected(features.error());
            result.features = std::move(*features);

            const auto utf8 = co_await askForUtf8(*connection);
            if (!utf8)
                co_return std::unexpected(utf8.error());
            result.utf8Accepted = *utf8;

            if (const auto binary =
                    co_await require(*connection, Command{.verb = Verb::RepresentationType, .argument = "I"});
                !binary)
                co_return std::unexpected(binary.error());

            auto home = co_await askHome(*connection);
            if (!home)
                co_return std::unexpected(home.error());
            result.homeDirectory = std::move(*home);
            co_return result;
        }
    }

    asio::awaitable<std::expected<LoginResult, RemoteIo::RemoteError>> logIn(
        std::shared_ptr<ControlConnection> connection,
        std::vector<asio::ip::tcp::endpoint> endpoints,
        LoginOptions options
    )
    {
        auto result = co_await establish(connection, std::move(endpoints), options);
        if (!result)
            connection->close();
        co_return result;
    }
}
