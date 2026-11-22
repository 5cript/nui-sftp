#include <ftp/client/control_connection.hpp>
#include <ftp/client/login.hpp>
#include <ftp/client/metadata.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/protocol/listing.hpp>
#include <ftp/client/transfer.hpp>
#include <ftp/support/scripted_ftp_server.hpp>
#include <ftp/support/test_pki.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <chrono>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        namespace asio = boost::asio;
        using namespace std::chrono_literals;
        using RemoteIo::RemoteErrorKind;
        using Result = std::expected<void, RemoteIo::RemoteError>;

        /**
         * @brief The control connection once the operation ended.
         */
        enum class Afterwards
        {
            /** @brief Open and in step with the server: a NOOP gets its own reply. */
            Idle,
            Dead,
            /** @brief Closed by the client, as a login does after an error. */
            Closed,
        };

        /**
         * @brief What the server is asked once the outcome of a row was sent.
         */
        enum class Then
        {
            /** @brief Nothing, the operation ended. */
            Ends,
            /** @brief What follows a success, see Route::afterSuccess. */
            GoesOn,
            /** @brief The questions about a refusal whose text tells no kind, see Route::explanation. */
            AsksWhy,
            /** @brief Another way to the same answer, see Route::fallback. */
            FallsBack,
        };

        /**
         * @brief What an operation of a route runs on.
         */
        struct Context
        {
            std::shared_ptr<ControlConnection> connection{};
            std::vector<asio::ip::tcp::endpoint> endpoints{};
            MetadataSession session{};
        };

        using Steps = std::function<void(Script&)>;

        void nothing(Script&)
        {}

        /**
         * @brief What a session learned before the operation: STAT is not asked, so a lookup lists the parent, and
         *        LIST -a works.
         */
        void listsParents(ServerDialect& dialect)
        {
            dialect.learnStatus(Support::Refused);
            dialect.learnListAll(Support::Works);
        }

        /**
         * @brief The way to one command: what the server does before it, the command, what follows its success, and
         *        the operation of the client that sends it.
         */
        struct Route
        {
            /** @brief The start of the names of its rows. */
            std::string name{};
            /** @brief drive() connects, else the test connects and reads the greeting first. */
            bool connectsItself{false};
            std::function<void(ServerDialect&)> learned{listsParents};
            /** @brief From the greeting up to the command. */
            Steps before{nothing};
            /** @brief The expect() of the command. */
            Steps command{};
            /** @brief The reply RFC 959 5.4 names for success. */
            Steps success{};
            Steps afterSuccess{nothing};
            Steps explanation{nothing};
            Steps fallback{nothing};
            std::function<asio::awaitable<Result>(Context)> drive{};
        };

        /**
         * @brief One command and one outcome, and what the client has to make of it.
         */
        struct Row
        {
            std::string name{};
            Route const* route{nullptr};
            /** @brief What the server does where the reply is due. */
            Steps outcome{};
            Then then{Then::Ends};
            /** @brief The server ends the connection or stalls, no step may follow. */
            bool serverEnds{false};
            /** @brief The command gets a short deadline. */
            bool hurried{false};
            /** @brief Nothing where the operation succeeds. */
            std::optional<RemoteErrorKind> kind{};
            Afterwards afterwards{Afterwards::Idle};
            /** @brief The reply code the error carries, 0 for none. */
            int code{0};
            /** @brief Text that the error shows the user. */
            std::string shown{};
        };

        void PrintTo(Row const& row, std::ostream* stream)
        {
            *stream << row.name;
        }

        Steps replying(std::string reply)
        {
            return [reply = std::move(reply)](Script& script)
            {
                script.reply(reply);
            };
        }

        int codeOf(std::string_view reply)
        {
            int code{0};
            std::from_chars(reply.data(), reply.data() + 3, code);
            return code;
        }

        /**
         * @brief The text of a one-line reply, without its code.
         */
        std::string textOf(std::string_view reply)
        {
            return std::string{reply.substr(4u)};
        }

        Row succeeds(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_Success", route.name),
                .route = &route,
                .outcome = route.success,
                .then = Then::GoesOn,
            };
        }

        /**
         * @brief Another 2xx than the one RFC 959 names, which the operation takes for a success.
         */
        Row completes(Route const& route, std::string reply)
        {
            return Row{
                .name = fmt::format("{}_OtherCompletion", route.name),
                .route = &route,
                .outcome = replying(std::move(reply)),
                .then = Then::GoesOn,
            };
        }

        /**
         * @brief A reply the operation cannot go on with, which still answers the command.
         */
        Row answersWrongly(Route const& route, std::string outcome, std::string reply, RemoteErrorKind kind)
        {
            const auto code = codeOf(reply);
            auto shown = textOf(reply);
            return Row{
                .name = fmt::format("{}_{}", route.name, outcome),
                .route = &route,
                .outcome = replying(std::move(reply)),
                .kind = kind,
                .code = code,
                .shown = std::move(shown),
            };
        }

        Row completesWrongly(Route const& route, std::string reply, RemoteErrorKind kind)
        {
            return answersWrongly(route, "OtherCompletion", std::move(reply), kind);
        }

        Row intermediate(Route const& route, std::string reply)
        {
            return answersWrongly(route, "Intermediate", std::move(reply), RemoteErrorKind::ProtocolViolation);
        }

        /**
         * @brief A 1yz to a command that takes none, then the success: the 1yz is no reply to wait for.
         */
        Row toleratesPreliminary(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_Preliminary", route.name),
                .route = &route,
                .outcome =
                    [&route](Script& script)
                {
                    script.reply("150 File status okay; about to open data connection.");
                    route.success(script);
                },
                .then = Then::GoesOn,
            };
        }

        Row refused(Route const& route, std::string reply, RemoteErrorKind kind, Then then = Then::Ends)
        {
            const auto code = codeOf(reply);
            auto shown = textOf(reply);
            return Row{
                .name = fmt::format("{}_Refused{}", route.name, code),
                .route = &route,
                .outcome = replying(std::move(reply)),
                .then = then,
                .kind = kind,
                .code = code,
                .shown = std::move(shown),
            };
        }

        /**
         * @brief A refusal whose text tells no kind, the operation asks the server what it meant.
         */
        Row refusedAndAsked(Route const& route, std::string reply, RemoteErrorKind kind)
        {
            return refused(route, std::move(reply), kind, Then::AsksWhy);
        }

        /**
         * @brief A refusal the operation goes past, it finds the answer another way.
         */
        Row refusedAndAvoided(Route const& route, std::string reply)
        {
            const auto code = codeOf(reply);
            return Row{
                .name = fmt::format("{}_Refused{}", route.name, code),
                .route = &route,
                .outcome = replying(std::move(reply)),
                .then = Then::FallsBack,
            };
        }

        /**
         * @brief A refusal the operation does without.
         */
        Row refusedAndIgnored(Route const& route, std::string reply)
        {
            const auto code = codeOf(reply);
            return Row{
                .name = fmt::format("{}_Refused{}", route.name, code),
                .route = &route,
                .outcome = replying(std::move(reply)),
            };
        }

        /**
         * @brief A refusal the operation goes on after as after a success.
         */
        Row refusedAndGoesOn(Route const& route, std::string reply)
        {
            const auto code = codeOf(reply);
            return Row{
                .name = fmt::format("{}_Refused{}", route.name, code),
                .route = &route,
                .outcome = replying(std::move(reply)),
                .then = Then::GoesOn,
            };
        }

        Row closesTheService(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_ServiceClosing", route.name),
                .route = &route,
                .outcome = replying("421 Service not available, closing control connection."),
                .kind = RemoteErrorKind::ConnectionLost,
                .afterwards = Afterwards::Dead,
                .code = 421,
                .shown = "Service not available",
            };
        }

        Row malformed(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_Malformed", route.name),
                .route = &route,
                .outcome =
                    [](Script& script)
                {
                    script.replyRaw("Command okay\r\n");
                },
                .kind = RemoteErrorKind::ProtocolViolation,
                .afterwards = Afterwards::Dead,
            };
        }

        Row endsTheStream(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_EndOfStream", route.name),
                .route = &route,
                .outcome =
                    [](Script& script)
                {
                    script.closeControl();
                },
                .serverEnds = true,
                .kind = RemoteErrorKind::ConnectionLost,
                .afterwards = Afterwards::Dead,
            };
        }

        Row resets(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_Reset", route.name),
                .route = &route,
                .outcome =
                    [](Script& script)
                {
                    script.resetControl();
                },
                .serverEnds = true,
                .kind = RemoteErrorKind::ConnectionLost,
                .afterwards = Afterwards::Dead,
            };
        }

        Row endsInAReply(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_PartialThenEnd", route.name),
                .route = &route,
                .outcome =
                    [](Script& script)
                {
                    script.replyRaw("250 Requested file act").closeControl();
                },
                .serverEnds = true,
                .kind = RemoteErrorKind::ConnectionLost,
                .afterwards = Afterwards::Dead,
                .code = 250,
                .shown = "in the middle of a reply",
            };
        }

        Row staysSilent(Route const& route)
        {
            return Row{
                .name = fmt::format("{}_Silence", route.name),
                .route = &route,
                .outcome =
                    [](Script& script)
                {
                    script.stall();
                },
                .serverEnds = true,
                .hurried = true,
                .kind = RemoteErrorKind::TimedOut,
                .afterwards = Afterwards::Dead,
            };
        }

        /**
         * @brief The outcomes that break the connection, the same for every command: 421, a line that is no reply,
         *        the end of the stream or a reset before the reply, part of a reply and then the end, no reply.
         */
        std::vector<Row> breakdowns(Route const& route)
        {
            return {
                closesTheService(route),
                malformed(route),
                endsTheStream(route),
                resets(route),
                endsInAReply(route),
                staysSilent(route),
            };
        }

        /**
         * @brief breakdowns() where every failure is reported as @p kind.
         */
        std::vector<Row> breakdowns(Route const& route, RemoteErrorKind kind)
        {
            auto rows = breakdowns(route);
            for (auto& row : rows)
                row.kind = kind;
            return rows;
        }

        /**
         * @brief @p row where the connection ends as @p afterwards.
         */
        Row leaving(Row row, Afterwards afterwards)
        {
            row.afterwards = afterwards;
            return row;
        }

        /**
         * @brief @p rows where the connection ends as @p afterwards.
         */
        std::vector<Row> leaving(std::vector<Row> rows, Afterwards afterwards)
        {
            for (auto& row : rows)
                row.afterwards = afterwards;
            return rows;
        }

        template <typename ValueT>
        Result withoutValue(std::expected<ValueT, RemoteIo::RemoteError> result)
        {
            if (!result)
                return std::unexpected(std::move(result).error());
            return Result{};
        }

        constexpr std::string_view listedFile = "-rw-r--r--    1 0        0               5 Jan 02 03:04 file\r\n";
        constexpr std::string_view listedSub = "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 sub\r\n";
        constexpr std::string_view listedNew = "drwxr-xr-x    2 0        0            4096 Jan 02 03:04 new\r\n";
        constexpr std::string_view listedRenamed =
            "-rw-r--r--    1 0        0               5 Jan 02 03:04 renamed\r\n";

        /**
         * @brief The LIST -a of @p directory, entered with CWD, whose data is @p lines.
         */
        Steps listing(std::string directory, std::string_view lines)
        {
            return [directory = std::move(directory), lines = std::string{lines}](Script& script)
            {
                script.expect("CWD", directory)
                    .reply("250 Directory successfully changed.")
                    .expect("EPSV")
                    .passive(DataPlan{}.send(lines))
                    .expect("LIST", "-a")
                    .reply("150 Here comes the directory listing.")
                    .beginData()
                    .awaitData()
                    .reply("226 Directory send OK.");
            };
        }

        Steps expecting(std::string verb, std::string argument)
        {
            return [verb = std::move(verb), argument = std::move(argument)](Script& script)
            {
                script.expect(verb, argument);
            };
        }

        Steps followedBy(Steps first, Steps second)
        {
            return [first = std::move(first), second = std::move(second)](Script& script)
            {
                first(script);
                second(script);
            };
        }

        Route const& changeDirectoryRoute()
        {
            static const Route route{
                .name = "Cwd",
                .command = expecting("CWD", "/dir/sub"),
                .success = replying("250 Directory successfully changed."),
                .afterSuccess = followedBy(expecting("PWD", ""), replying("257 \"/dir/sub\" is the current directory")),
                .explanation = listing("/dir", listedSub),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return withoutValue(co_await canonicalDirectory(context.session, "/dir/sub"));
                },
            };
            return route;
        }

        Route const& printWorkingDirectoryRoute()
        {
            static const Route route{
                .name = "Pwd",
                .before = followedBy(expecting("CWD", "/dir/sub"), replying("250 Directory successfully changed.")),
                .command = expecting("PWD", ""),
                .success = replying("257 \"/dir/sub\" is the current directory"),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return withoutValue(co_await canonicalDirectory(context.session, "/dir/sub"));
                },
            };
            return route;
        }

        Route const& makeDirectoryRoute()
        {
            static const Route route{
                .name = "Mkd",
                .command = expecting("MKD", "/dir/new"),
                .success = replying("257 \"/dir/new\" created"),
                .explanation = listing("/dir", listedNew),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await FileTransferProtocol::makeDirectory(context.session, "/dir/new");
                },
            };
            return route;
        }

        Route const& removeDirectoryRoute()
        {
            static const Route route{
                .name = "Rmd",
                .before = followedBy(expecting("CWD", "/dir"), replying("250 Directory successfully changed.")),
                .command = expecting("RMD", "/dir/sub"),
                .success = replying("250 Remove directory operation successful."),
                .explanation = followedBy(listing("/dir", listedSub), listing("/dir/sub", "")),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await FileTransferProtocol::removeDirectory(context.session, "/dir/sub");
                },
            };
            return route;
        }

        Route const& deleteRoute()
        {
            static const Route route{
                .name = "Dele",
                .command = expecting("DELE", "/dir/file"),
                .success = replying("250 Delete operation successful."),
                .explanation = listing("/dir", listedFile),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await removeFile(context.session, "/dir/file");
                },
            };
            return route;
        }

        Route const& renameFromRoute()
        {
            static const Route route{
                .name = "Rnfr",
                .command = expecting("RNFR", "/dir/file"),
                .success = replying("350 Ready for RNTO."),
                .afterSuccess = followedBy(expecting("RNTO", "/dir/renamed"), replying("250 Rename successful.")),
                .explanation = listing("/dir", listedFile),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await renamePath(context.session, "/dir/file", "/dir/renamed");
                },
            };
            return route;
        }

        Route const& renameToRoute()
        {
            static const Route route{
                .name = "Rnto",
                .before = followedBy(expecting("RNFR", "/dir/file"), replying("350 Ready for RNTO.")),
                .command = expecting("RNTO", "/dir/renamed"),
                .success = replying("250 Rename successful."),
                .explanation = listing("/dir", listedRenamed),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await renamePath(context.session, "/dir/file", "/dir/renamed");
                },
            };
            return route;
        }

        Route const& sizeRoute()
        {
            static const Route route{
                .name = "Size",
                .command = expecting("SIZE", "/dir/file"),
                .success = replying("213 5"),
                .explanation = listing("/dir", listedFile),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return withoutValue(co_await sizeOf(context.session, "/dir/file"));
                },
            };
            return route;
        }

        Route const& modificationTimeRoute()
        {
            static const Route route{
                .name = "Mdtm",
                .before = followedBy(expecting("SIZE", "/dir/file"), replying("213 5")),
                .command = expecting("MDTM", "/dir/file"),
                .success = replying("213 20260102030405"),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return withoutValue(co_await statPath(context.session, "/dir/file"));
                },
            };
            return route;
        }

        constexpr auto someTime = std::chrono::sys_days{std::chrono::year{2026} / 1 / 2} + 3h + 4min + 5s;

        Route setterRoute(std::string name, ModificationTimeSetter setter, Steps command, Steps success)
        {
            return Route{
                .name = std::move(name),
                .learned =
                    [setter](ServerDialect& dialect)
                {
                    listsParents(dialect);
                    dialect.learnModificationTimeSetter(setter);
                },
                .command = std::move(command),
                .success = std::move(success),
                .explanation = listing("/dir", listedFile),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await setModificationTime(context.session, "/dir/file", someTime);
                },
            };
        }

        Route const& modifyFactRoute()
        {
            static const Route route = setterRoute(
                "Mfmt",
                ModificationTimeSetter::ModifyFact,
                expecting("MFMT", "20260102030405 /dir/file"),
                replying("213 Modify=20260102030405; /dir/file")
            );
            return route;
        }

        Route const& siteUtimeRoute()
        {
            static const Route route = setterRoute(
                "SiteUtime",
                ModificationTimeSetter::SiteUtime,
                expecting("SITE", "UTIME 20260102030405 /dir/file"),
                replying("200 SITE UTIME command successful")
            );
            return route;
        }

        Route const& statusRoute()
        {
            static const Route route{
                .name = "Stat",
                .learned =
                    [](ServerDialect& dialect)
                {
                    dialect.learnListAll(Support::Works);
                },
                .command = expecting("STAT", "/dir/file"),
                .success = replying(
                    "213-Status of /dir/file:\n-rw-r--r--   1 0        0               5 Jan 02 03:04 /dir/file\n213 End"
                ),
                .fallback = listing("/dir", listedFile),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return withoutValue(co_await lstatPath(context.session, "/dir/file"));
                },
            };
            return route;
        }

        TlsConnectionSettings tlsSettings()
        {
            const auto pki = testPki();
            if (!pki)
                throw std::runtime_error{pki.error()};
            auto context = TlsContext::create({.systemAuthorities = false, .authorities = {pki->get().authority}});
            if (!context)
                throw std::runtime_error{context.error().toString()};
            return TlsConnectionSettings{.context = std::move(context).value(), .serverName = "127.0.0.1"};
        }

        /**
         * @brief One command of a login, with the reply that lets the login go on.
         */
        struct LoginStep
        {
            std::string verb{};
            std::string argument{};
            Steps success{};
        };

        Steps afterAuthentication()
        {
            return [](Script& script)
            {
                script.featLike(ServerLike::Vsftpd)
                    .expect("OPTS", "UTF8 ON")
                    .reply("200 Always in UTF8 mode.")
                    .expect("TYPE", "I")
                    .reply("200 Switching to Binary mode.")
                    .expect("PWD", "")
                    .reply("257 \"/home/user\" is the current directory");
            };
        }

        std::vector<LoginStep> loginSteps(bool secured, bool withAccount)
        {
            std::vector<LoginStep> steps{};
            if (secured)
            {
                steps.push_back(
                    {"AUTH",
                        "TLS",
                        [](Script& script)
                        {
                            script.reply("234 Proceed with negotiation.").startTls();
                        }}
                );
            }
            steps.push_back({"USER", "user", replying("331 Please specify the password.")});
            if (withAccount)
            {
                steps.push_back({"PASS", "password", replying("332 Need account for login.")});
                steps.push_back({"ACCT", "account", replying("230 Login successful.")});
            }
            else
                steps.push_back({"PASS", "password", replying("230 Login successful.")});
            if (secured)
            {
                steps.push_back({"PBSZ", "0", replying("200 PBSZ set to 0.")});
                steps.push_back({"PROT", "P", replying("200 PROT now Private.")});
            }
            steps.push_back(
                {"FEAT",
                    "",
                    [](Script& script)
                    {
                        script.reply(
                            "211-Features:\n EPRT\n EPSV\n MDTM\n PASV\n PBSZ\n PROT\n REST STREAM\n SIZE\n TVFS\n211 End"
                        );
                    }}
            );
            steps.push_back({"OPTS", "UTF8 ON", replying("200 Always in UTF8 mode.")});
            steps.push_back({"TYPE", "I", replying("200 Switching to Binary mode.")});
            steps.push_back({"PWD", "", replying("257 \"/home/user\" is the current directory")});
            return steps;
        }

        /**
         * @brief The steps of @p steps from @p first up to @p last, without it.
         */
        Steps loginStepsBetween(std::vector<LoginStep> const& steps, std::size_t first, std::size_t last)
        {
            return [steps = std::vector<LoginStep>{steps.begin() + first, steps.begin() + last}](Script& script)
            {
                for (auto const& step : steps)
                {
                    script.expect(step.verb, step.argument);
                    step.success(script);
                }
            };
        }

        /**
         * @brief The route to @p verb in a login, plain or over explicit TLS, with an account where @p withAccount.
         */
        Route loginRoute(std::string name, std::string_view verb, bool secured, bool withAccount = false)
        {
            const auto steps = loginSteps(secured, withAccount);
            std::size_t index{0u};
            while (index < steps.size() && steps[index].verb != verb)
                ++index;
            if (index == steps.size())
                throw std::logic_error{fmt::format("A login sends no {}", verb)};
            return Route{
                .name = std::move(name),
                .connectsItself = true,
                .before = loginStepsBetween(steps, 0u, index),
                .command = expecting(steps[index].verb, steps[index].argument),
                .success = steps[index].success,
                .afterSuccess = loginStepsBetween(steps, index + 1u, steps.size()),
                .drive = [secured, withAccount](Context context) -> asio::awaitable<Result>
                {
                    LoginOptions options{.credentials = {.user = "user", .password = "password"}};
                    if (withAccount)
                        options.credentials.account = "account";
                    if (secured)
                    {
                        options.tlsMode = TlsMode::Explicit;
                        options.tls = tlsSettings();
                    }
                    co_return withoutValue(co_await logIn(context.connection, context.endpoints, std::move(options)));
                },
            };
        }

        Route const& userRoute()
        {
            static const Route route = loginRoute("User", "USER", false);
            return route;
        }

        Route const& passwordRoute()
        {
            static const Route route = loginRoute("Pass", "PASS", false);
            return route;
        }

        Route const& accountRoute()
        {
            static const Route route = loginRoute("Acct", "ACCT", false, true);
            return route;
        }

        Route const& authenticateRoute()
        {
            static const Route route = loginRoute("Auth", "AUTH", true);
            return route;
        }

        Route const& protectionBufferSizeRoute()
        {
            static const Route route = loginRoute("Pbsz", "PBSZ", true);
            return route;
        }

        Route const& dataProtectionRoute()
        {
            static const Route route = loginRoute("Prot", "PROT", true);
            return route;
        }

        Route const& featuresRoute()
        {
            static const Route route = loginRoute("Feat", "FEAT", false);
            return route;
        }

        Route const& optionsRoute()
        {
            static const Route route = loginRoute("Opts", "OPTS", false);
            return route;
        }

        Route const& representationTypeRoute()
        {
            static const Route route = loginRoute("Type", "TYPE", false);
            return route;
        }

        Route const& homeDirectoryRoute()
        {
            static const Route route = loginRoute("LoginPwd", "PWD", false);
            return route;
        }

        /**
         * @brief @p row under the name of @p outcome.
         */
        Row named(Row row, std::string_view outcome)
        {
            row.name = fmt::format("{}_{}", row.route->name, outcome);
            return row;
        }

        void append(std::vector<Row>& rows, std::vector<Row> more)
        {
            for (auto& row : more)
                rows.push_back(std::move(row));
        }

        std::vector<Row> metadataRows()
        {
            std::vector<Row> rows{};
            {
                auto const& route = changeDirectoryRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "200 Command okay."),
                        toleratesPreliminary(route),
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::PermissionDenied
                        ),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(route, "550 /dir/sub: No such file or directory", RemoteErrorKind::NoSuchFile),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = printWorkingDirectoryRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completesWrongly(route, "200 Command okay.", RemoteErrorKind::ProtocolViolation),
                        toleratesPreliminary(route),
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refused(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "550 Permission denied", RemoteErrorKind::PermissionDenied),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = makeDirectoryRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "250 Directory created."),
                        toleratesPreliminary(route),
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::AlreadyExists
                        ),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(
                            route, "521 \"/dir/new\" directory already exists", RemoteErrorKind::AlreadyExists
                        ),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(route, "550 /dir/new: Permission denied", RemoteErrorKind::PermissionDenied),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = removeDirectoryRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "200 Command okay."),
                        toleratesPreliminary(route),
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::PermissionDenied
                        ),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(route, "550 /dir/sub: Permission denied", RemoteErrorKind::PermissionDenied),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = deleteRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "200 Command okay."),
                        toleratesPreliminary(route),
                        intermediate(route, "350 Requested file action pending further information."),
                        refusedAndAsked(
                            route, "450 Requested file action not taken.", RemoteErrorKind::PermissionDenied
                        ),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::PermissionDenied
                        ),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(route, "550 /dir/file: No such file or directory", RemoteErrorKind::NoSuchFile),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = renameFromRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        // RNFR goes on with 350 alone (RFC 959 5.4).
                        completesWrongly(route, "200 Command okay.", RemoteErrorKind::ProtocolViolation),
                        toleratesPreliminary(route),
                        intermediate(route, "331 User name okay, need password."),
                        refusedAndAsked(
                            route, "450 Requested file action not taken.", RemoteErrorKind::PermissionDenied
                        ),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::PermissionDenied
                        ),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(route, "550 /dir/file: No such file or directory", RemoteErrorKind::NoSuchFile),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = renameToRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "200 Command okay."),
                        toleratesPreliminary(route),
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::AlreadyExists
                        ),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "503 Bad sequence of commands.", RemoteErrorKind::ProtocolViolation),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(route, "532 Need account for storing files.", RemoteErrorKind::PermissionDenied),
                        refused(route, "553 Rename failed.", RemoteErrorKind::PermissionDenied),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = sizeRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completesWrongly(route, "250 Requested file action okay.", RemoteErrorKind::ProtocolViolation),
                        toleratesPreliminary(route),
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::PermissionDenied
                        ),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "504 Command not implemented for that parameter.", RemoteErrorKind::Unsupported),
                        refused(route, "550 /dir/file: No such file or directory", RemoteErrorKind::NoSuchFile),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                // The time of statPath() is optional, it does without it.
                auto const& route = modificationTimeRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "250 Requested file action okay."),
                        toleratesPreliminary(route),
                        named(
                            refusedAndIgnored(route, "350 Requested file action pending further information."),
                            "Intermediate"
                        ),
                        refusedAndIgnored(route, "500 Syntax error, command unrecognized."),
                        refusedAndIgnored(route, "501 Syntax error in parameters or arguments."),
                        refusedAndIgnored(route, "502 Command not implemented."),
                        refusedAndIgnored(route, "504 Command not implemented for that parameter."),
                        refusedAndIgnored(route, "550 /dir/file: Permission denied"),
                    }
                );
                // The 421 is the reply MDTM waited for, the size is known and the connection ends.
                auto failures = breakdowns(route);
                failures.front().kind.reset();
                append(rows, std::move(failures));
            }
            for (auto const* route : {&modifyFactRoute(), &siteUtimeRoute()})
            {
                append(
                    rows,
                    {
                        succeeds(*route),
                        toleratesPreliminary(*route),
                        intermediate(*route, "350 Requested file action pending further information."),
                        refused(
                            *route, "451 Requested action aborted: local error in processing.", RemoteErrorKind::Failure
                        ),
                        refused(*route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refused(*route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Unsupported),
                        refused(*route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(*route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(*route, "550 /dir/file: No such file or directory", RemoteErrorKind::NoSuchFile),
                    }
                );
                append(rows, breakdowns(*route));
            }
            append(rows, {completes(modifyFactRoute(), "250 Modified.")});
            // RFC 959 4.2: 202 is a command not implemented, the time stays as it was.
            append(
                rows,
                {completesWrongly(
                    siteUtimeRoute(), "202 Command not implemented, superfluous at this site.", RemoteErrorKind::Unsupported
                )}
            );
            {
                // Every refusal of STAT leaves the listing of the parent.
                auto const& route = statusRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        Row{
                            .name = "Stat_OtherCompletion",
                            .route = &route,
                            .outcome = replying("211 System status, or system help reply."),
                            .then = Then::FallsBack,
                        },
                        toleratesPreliminary(route),
                        named(
                            refusedAndAvoided(route, "350 Requested file action pending further information."),
                            "Intermediate"
                        ),
                        refusedAndAvoided(route, "450 Requested file action not taken."),
                        refusedAndAvoided(route, "500 Syntax error, command unrecognized."),
                        refusedAndAvoided(route, "501 Syntax error in parameters or arguments."),
                        refusedAndAvoided(route, "502 Command not implemented."),
                        refusedAndAvoided(route, "530 Not logged in."),
                    }
                );
                append(rows, breakdowns(route));
            }
            return rows;
        }

        /**
         * @brief A login closes its connection after an error.
         */
        Row refusedAtLogin(Route const& route, std::string reply, RemoteErrorKind kind)
        {
            return leaving(refused(route, std::move(reply), kind), Afterwards::Closed);
        }

        std::vector<Row> loginRows()
        {
            std::vector<Row> rows{};
            {
                auto const& route = userRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        // A 230 needs no password.
                        Row{
                            .name = "User_OtherCompletion",
                            .route = &route,
                            .outcome = followedBy(replying("230 Login successful."), afterAuthentication()),
                        },
                        toleratesPreliminary(route),
                        leaving(
                            intermediate(route, "350 Requested file action pending further information."),
                            Afterwards::Closed
                        ),
                        Row{
                            .name = "User_NeedsAccount",
                            .route = &route,
                            .outcome = replying("332 Need account for login."),
                            .kind = RemoteErrorKind::AuthenticationFailed,
                            .afterwards = Afterwards::Closed,
                            .shown = "none is set",
                        },
                        refusedAtLogin(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAtLogin(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refusedAtLogin(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        named(
                            refusedAtLogin(
                                route,
                                "530 Non-anonymous sessions must use encryption.",
                                RemoteErrorKind::NoSecureConnection
                            ),
                            "Refused530DemandingTls"
                        ),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = passwordRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        // RFC 959 5.4: no password was needed.
                        completes(route, "202 Command not implemented, superfluous at this site."),
                        toleratesPreliminary(route),
                        leaving(
                            intermediate(route, "350 Requested file action pending further information."),
                            Afterwards::Closed
                        ),
                        Row{
                            .name = "Pass_NeedsAccount",
                            .route = &route,
                            .outcome = replying("332 Need account for login."),
                            .kind = RemoteErrorKind::AuthenticationFailed,
                            .afterwards = Afterwards::Closed,
                            .shown = "none is set",
                        },
                        refusedAtLogin(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAtLogin(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refusedAtLogin(route, "503 Bad sequence of commands.", RemoteErrorKind::ProtocolViolation),
                        refusedAtLogin(route, "530 Login incorrect.", RemoteErrorKind::AuthenticationFailed),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = accountRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "202 Command not implemented, superfluous at this site."),
                        toleratesPreliminary(route),
                        leaving(
                            intermediate(route, "350 Requested file action pending further information."),
                            Afterwards::Closed
                        ),
                        refusedAtLogin(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAtLogin(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refusedAtLogin(route, "503 Bad sequence of commands.", RemoteErrorKind::ProtocolViolation),
                        refusedAtLogin(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                // Whatever keeps TLS from starting ends the connection, it never goes on in plaintext.
                auto const& route = authenticateRoute();
                const auto refusedTls = [&route](std::string reply)
                {
                    return leaving(refused(route, std::move(reply), RemoteErrorKind::NoSecureConnection), Afterwards::Dead);
                };
                append(
                    rows,
                    {
                        succeeds(route),
                        // RFC 2228 3: 334 asks for ADAT, curl and the client take it for a 234.
                        Row{
                            .name = "Auth_Intermediate334",
                            .route = &route,
                            .outcome =
                                [](Script& script)
                            {
                                script.reply("334 ADAT=").startTls();
                            },
                            .then = Then::GoesOn,
                        },
                        leaving(
                            completesWrongly(route, "200 Command okay.", RemoteErrorKind::NoSecureConnection),
                            Afterwards::Dead
                        ),
                        toleratesPreliminary(route),
                        refusedTls("421 Service not available, closing control connection."),
                        refusedTls("431 Need some unavailable resource to process security."),
                        refusedTls("500 Syntax error, command unrecognized."),
                        refusedTls("501 Syntax error in parameters or arguments."),
                        refusedTls("502 Command not implemented."),
                        refusedTls("504 Command not implemented for that parameter."),
                        refusedTls("533 Command protection level denied for policy reasons."),
                        refusedTls("534 Request denied for policy reasons."),
                    }
                );
                auto failures = breakdowns(route, RemoteErrorKind::NoSecureConnection);
                // The 421 is a refusal above.
                failures.erase(failures.begin());
                append(rows, std::move(failures));
            }
            for (auto const* route : {&protectionBufferSizeRoute(), &dataProtectionRoute()})
            {
                // Without protected data connections the session is not secure.
                const auto refusedProtection = [route](std::string reply)
                {
                    return refusedAtLogin(*route, std::move(reply), RemoteErrorKind::NoSecureConnection);
                };
                append(
                    rows,
                    {
                        succeeds(*route),
                        completes(*route, "220 Command okay."),
                        toleratesPreliminary(*route),
                        leaving(
                            intermediate(*route, "350 Requested file action pending further information."),
                            Afterwards::Closed
                        ),
                        refusedProtection("500 Syntax error, command unrecognized."),
                        refusedProtection("501 Syntax error in parameters or arguments."),
                        refusedProtection("502 Command not implemented."),
                        refusedProtection("503 Bad sequence of commands."),
                        refusedProtection("530 Not logged in."),
                        refusedProtection("533 Command protection level denied for policy reasons."),
                    }
                );
                for (auto& row : rows)
                {
                    if (row.route == route && row.kind == RemoteErrorKind::ProtocolViolation)
                        row.kind = RemoteErrorKind::NoSecureConnection;
                }
                append(rows, breakdowns(*route, RemoteErrorKind::NoSecureConnection));
            }
            append(
                rows,
                {
                    refusedAtLogin(
                        dataProtectionRoute(),
                        "504 Command not implemented for that parameter.",
                        RemoteErrorKind::NoSecureConnection
                    ),
                    refusedAtLogin(
                        dataProtectionRoute(),
                        "534 Request denied for policy reasons.",
                        RemoteErrorKind::NoSecureConnection
                    ),
                    refusedAtLogin(
                        dataProtectionRoute(),
                        "536 Requested PROT level not supported by mechanism.",
                        RemoteErrorKind::NoSecureConnection
                    ),
                    refusedAtLogin(
                        dataProtectionRoute(),
                        "431 Need some unavailable resource to process security.",
                        RemoteErrorKind::NoSecureConnection
                    ),
                }
            );
            for (auto const* route : {&featuresRoute(), &optionsRoute()})
            {
                // A server without FEAT has no features (RFC 2389 3), one that refuses UTF8 may use it anyway.
                append(
                    rows,
                    {
                        succeeds(*route),
                        toleratesPreliminary(*route),
                        refusedAndGoesOn(*route, "500 Syntax error, command unrecognized."),
                        refusedAndGoesOn(*route, "501 Syntax error in parameters or arguments."),
                        refusedAndGoesOn(*route, "502 Command not implemented."),
                    }
                );
                append(rows, breakdowns(*route));
            }
            append(
                rows,
                {
                    // RFC 2389 3.2: FEAT is answered with 211 or refused.
                    leaving(
                        completesWrongly(featuresRoute(), "200 Command okay.", RemoteErrorKind::ProtocolViolation),
                        Afterwards::Closed
                    ),
                    leaving(
                        intermediate(featuresRoute(), "350 Requested file action pending further information."),
                        Afterwards::Closed
                    ),
                    completes(optionsRoute(), "202 Command not implemented, superfluous at this site."),
                    named(
                        refusedAndGoesOn(optionsRoute(), "350 Requested file action pending further information."),
                        "Intermediate"
                    ),
                    refusedAndGoesOn(optionsRoute(), "451 Requested action aborted. Local error in processing."),
                }
            );
            {
                auto const& route = representationTypeRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "250 Requested file action okay."),
                        toleratesPreliminary(route),
                        leaving(
                            intermediate(route, "350 Requested file action pending further information."),
                            Afterwards::Closed
                        ),
                        refusedAtLogin(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAtLogin(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refusedAtLogin(
                            route, "504 Command not implemented for that parameter.", RemoteErrorKind::Unsupported
                        ),
                        refusedAtLogin(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = homeDirectoryRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        leaving(
                            completesWrongly(route, "200 Command okay.", RemoteErrorKind::ProtocolViolation),
                            Afterwards::Closed
                        ),
                        toleratesPreliminary(route),
                        leaving(
                            intermediate(route, "350 Requested file action pending further information."),
                            Afterwards::Closed
                        ),
                        refusedAtLogin(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAtLogin(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refusedAtLogin(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refusedAtLogin(route, "550 Permission denied", RemoteErrorKind::PermissionDenied),
                    }
                );
                append(rows, breakdowns(route));
            }
            return rows;
        }

        /**
         * @brief The first reply @p preliminary, the data of the plan begun before, then @p final.
         */
        Steps withData(std::string preliminary, Steps final)
        {
            return [preliminary = std::move(preliminary), final = std::move(final)](Script& script)
            {
                script.reply(preliminary).beginData().awaitData();
                final(script);
            };
        }

        Steps passiveWith(std::string verb, DataPlan plan)
        {
            return [verb = std::move(verb), plan = std::move(plan)](Script& script)
            {
                script.expect(verb).passive(plan);
            };
        }

        asio::awaitable<Result> downloadWhole(Context context, DownloadRequest request)
        {
            auto started = co_await Download::start(context.session.transfer, std::move(request), context.session.options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            auto const download = std::move(started).value();
            std::array<char, 64u> buffer{};
            std::expected<std::size_t, RemoteIo::RemoteError> read = std::size_t{1u};
            while (read && *read != 0u)
                read = co_await download->readSome(asio::buffer(buffer));
            if (!read)
                co_return std::unexpected(std::move(read).error());
            co_return withoutValue(co_await download->finish());
        }

        asio::awaitable<Result> uploadWhole(Context context, UploadRequest request)
        {
            auto started = co_await Upload::start(context.session.transfer, std::move(request), context.session.options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            auto const upload = std::move(started).value();
            constexpr std::string_view content = "hello";
            if (auto written = co_await upload->write(asio::buffer(content)); !written)
                co_return std::unexpected(std::move(written).error());
            co_return withoutValue(co_await upload->finish());
        }

        asio::awaitable<Result> listWith(Context context, Verb verb)
        {
            auto parser = verb == Verb::MachineListDirectory
                ? ListingParser::machineListing()
                : ListingParser::list(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
            co_return withoutValue(
                co_await fetchListing(
                    context.session.transfer, Command{.verb = verb}, std::move(parser), context.session.options
                )
            );
        }

        constexpr std::string_view downloadAnnounced = "150 Opening BINARY mode data connection for /dir/file (5 bytes).";
        constexpr std::string_view listingAnnounced = "150 Here comes the directory listing.";
        constexpr std::string_view uploadAnnounced = "150 Ok to send data.";
        constexpr std::string_view machineListedFile = "type=file;size=5;modify=20260102030405; file\r\n";

        Steps listingAfterPassive(std::string verb)
        {
            return followedBy(
                expecting(std::move(verb), ""),
                withData(std::string{listingAnnounced}, replying("226 Directory send OK."))
            );
        }

        Route const& extendedPassiveRoute()
        {
            static const Route route{
                .name = "Epsv",
                .command = expecting("EPSV", ""),
                .success =
                    [](Script& script)
                {
                    script.passive(DataPlan{}.send(std::string{listedFile}));
                },
                .afterSuccess = listingAfterPassive("LIST"),
                .fallback = followedBy(passiveWith("PASV", DataPlan{}.send(std::string{listedFile})), listingAfterPassive("LIST")),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await listWith(std::move(context), Verb::List);
                },
            };
            return route;
        }

        Route const& passiveRoute()
        {
            static const Route route{
                .name = "Pasv",
                .before = followedBy(expecting("EPSV", ""), replying("502 Command not implemented.")),
                .command = expecting("PASV", ""),
                .success =
                    [](Script& script)
                {
                    script.passive(DataPlan{}.send(std::string{listedFile}));
                },
                .afterSuccess = listingAfterPassive("LIST"),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await listWith(std::move(context), Verb::List);
                },
            };
            return route;
        }

        Route const& restartRoute()
        {
            static const Route route{
                .name = "Rest",
                .before = passiveWith("EPSV", DataPlan{}.send("llo")),
                .command = expecting("REST", "2"),
                .success = replying("350 Restart position accepted (2)."),
                .afterSuccess = followedBy(
                    expecting("RETR", "/dir/file"),
                    withData("150 Opening BINARY mode data connection for /dir/file (5 bytes).", replying("226 Transfer complete."))
                ),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await downloadWhole(std::move(context), DownloadRequest{.path = "/dir/file", .offset = 2u});
                },
            };
            return route;
        }

        Route const& retrieveRoute()
        {
            static const Route route{
                .name = "Retr",
                .before = passiveWith("EPSV", DataPlan{}.send("hello")),
                .command = expecting("RETR", "/dir/file"),
                .success = withData(std::string{downloadAnnounced}, replying("226 Transfer complete.")),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await downloadWhole(std::move(context), DownloadRequest{.path = "/dir/file"});
                },
            };
            return route;
        }

        Route uploadRoute(std::string name, std::string verb, UploadMode mode)
        {
            return Route{
                .name = std::move(name),
                .before = passiveWith("EPSV", DataPlan{}.expectUpload(100u)),
                .command = expecting(std::move(verb), "/dir/file"),
                .success = withData(std::string{uploadAnnounced}, replying("226 Transfer complete.")),
                .drive = [mode](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await uploadWhole(std::move(context), UploadRequest{.path = "/dir/file", .mode = mode});
                },
            };
        }

        Route const& storeRoute()
        {
            static const Route route = uploadRoute("Stor", "STOR", UploadMode::Store);
            return route;
        }

        Route const& appendRoute()
        {
            static const Route route = uploadRoute("Appe", "APPE", UploadMode::Append);
            return route;
        }

        Route listingRoute(std::string name, std::string verb, Verb sent, std::string_view data)
        {
            return Route{
                .name = std::move(name),
                .before = passiveWith("EPSV", DataPlan{}.send(std::string{data})),
                .command = expecting(std::move(verb), ""),
                .success = withData(std::string{listingAnnounced}, replying("226 Directory send OK.")),
                .drive = [sent](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await listWith(std::move(context), sent);
                },
            };
        }

        Route const& listRoute()
        {
            static const Route route = listingRoute("List", "LIST", Verb::List, listedFile);
            return route;
        }

        Route const& machineListRoute()
        {
            static const Route route = listingRoute("Mlsd", "MLSD", Verb::MachineListDirectory, machineListedFile);
            return route;
        }

        /**
         * @brief What a transfer whose control connection ended without the final reply is.
         */
        enum class WithoutFinalReply
        {
            /** @brief Complete, all the data the server announced is proof enough. */
            Complete,
            /** @brief Unconfirmed, TimedOut: only the size on the server tells. */
            SizeTells,
            /** @brief Failed with the end of the control connection. */
            Fails,
        };

        /**
         * @brief The outcomes of the reply that ends a transfer once its data ended, named Final....
         */
        std::vector<Row> finalReplies(Route const& route, std::string preliminary, WithoutFinalReply missing)
        {
            const auto after = [&route, &preliminary](std::string name, Steps final)
            {
                return Row{
                    .name = fmt::format("{}_Final{}", route.name, name),
                    .route = &route,
                    .outcome = withData(preliminary, std::move(final)),
                };
            };
            auto refusedAfter = [&after](std::string reply, RemoteErrorKind kind)
            {
                const auto code = codeOf(reply);
                auto row = after(fmt::format("Refused{}", code), replying(reply));
                row.kind = kind;
                row.code = code;
                row.shown = textOf(reply);
                return row;
            };
            auto closing = refusedAfter("421 Service not available, closing control connection.", RemoteErrorKind::ConnectionLost);
            closing.name = fmt::format("{}_FinalServiceClosing", route.name);
            closing.afterwards = Afterwards::Dead;
            auto ended = after("EndOfStream", [](Script& script) { script.closeControl(); });
            ended.serverEnds = true;
            ended.afterwards = Afterwards::Dead;
            auto silent = after("Silence", [](Script& script) { script.stall(); });
            silent.serverEnds = true;
            silent.hurried = true;
            silent.afterwards = Afterwards::Dead;
            if (missing == WithoutFinalReply::SizeTells)
                ended.kind = RemoteErrorKind::TimedOut;
            if (missing == WithoutFinalReply::Fails)
                ended.kind = RemoteErrorKind::ConnectionLost;
            if (missing != WithoutFinalReply::Complete)
                silent.kind = RemoteErrorKind::TimedOut;
            return {
                after("Completion250", replying("250 Requested file action okay, completed.")),
                refusedAfter("426 Connection closed; transfer aborted.", RemoteErrorKind::ConnectionLost),
                refusedAfter("451 Requested action aborted: local error in processing.", RemoteErrorKind::Failure),
                closing,
                ended,
                silent,
            };
        }

        /**
         * @brief The outcomes of the first reply to a transfer command that every one of them shares.
         */
        std::vector<Row> firstReplies(Route const& route, Steps data)
        {
            return {
                succeeds(route),
                Row{
                    .name = fmt::format("{}_Preliminary125", route.name),
                    .route = &route,
                    .outcome = withData("125 Data connection already open; transfer starting.", replying("226 Transfer complete.")),
                },
                // RFC 959 5.4 lists no 2xx before the data, servers send the final reply early.
                Row{
                    .name = fmt::format("{}_OtherCompletion", route.name),
                    .route = &route,
                    .outcome = followedBy(replying("226 Transfer complete."), std::move(data)),
                },
                refused(route, "425 Can't open data connection.", RemoteErrorKind::NoConnection),
                refused(route, "426 Connection closed; transfer aborted.", RemoteErrorKind::ConnectionLost),
                refused(route, "450 Requested file action not taken.", RemoteErrorKind::Failure),
                refused(route, "451 Requested action aborted: local error in processing.", RemoteErrorKind::Failure),
                refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                refused(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                // The data connection would need another protection level, RFC 4217 10.2 and RFC 2228 6.
                refused(
                    route,
                    "521 Data connection cannot be opened with this PROT setting.",
                    RemoteErrorKind::NoSecureConnection
                ),
                // What vsftpd sends.
                refused(route, "522 Data connections must be encrypted.", RemoteErrorKind::NoSecureConnection),
                refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                refused(route, "534 Request denied for policy reasons.", RemoteErrorKind::NoSecureConnection),
            };
        }

        Steps dataOnly()
        {
            return [](Script& script)
            {
                script.beginData().awaitData();
            };
        }

        std::vector<Row> transferRows()
        {
            std::vector<Row> rows{};
            for (auto const* route : {&extendedPassiveRoute(), &passiveRoute()})
            {
                append(
                    rows,
                    {
                        succeeds(*route),
                        toleratesPreliminary(*route),
                    }
                );
                append(rows, breakdowns(*route));
            }
            {
                // A data connection EPSV cannot give, PASV may (RFC 2428 3).
                auto const& route = extendedPassiveRoute();
                append(
                    rows,
                    {
                        named(refusedAndAvoided(route, "200 Command okay."), "OtherCompletion"),
                        named(
                            refusedAndAvoided(route, "350 Requested file action pending further information."),
                            "Intermediate"
                        ),
                        refusedAndAvoided(route, "500 Syntax error, command unrecognized."),
                        refusedAndAvoided(route, "501 Syntax error in parameters or arguments."),
                        refusedAndAvoided(route, "502 Command not implemented."),
                        refusedAndAvoided(route, "530 Not logged in."),
                    }
                );
            }
            {
                auto const& route = passiveRoute();
                append(
                    rows,
                    {
                        completesWrongly(route, "200 Command okay.", RemoteErrorKind::ProtocolViolation),
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refused(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                    }
                );
            }
            {
                auto const& route = restartRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        completesWrongly(route, "200 Command okay.", RemoteErrorKind::ProtocolViolation),
                        toleratesPreliminary(route),
                        intermediate(route, "331 User name okay, need password."),
                        refused(route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refused(route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::Failure),
                        refused(route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                        refused(route, "530 Not logged in.", RemoteErrorKind::AuthenticationFailed),
                        refused(route, "554 Requested action not taken: invalid REST parameter.", RemoteErrorKind::Failure),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = retrieveRoute();
                append(rows, firstReplies(route, dataOnly()));
                append(
                    rows,
                    {
                        intermediate(route, "350 Requested file action pending further information."),
                        refused(route, "550 /dir/file: No such file or directory", RemoteErrorKind::NoSuchFile),
                    }
                );
                append(rows, finalReplies(route, std::string{downloadAnnounced}, WithoutFinalReply::Complete));
                append(rows, breakdowns(route));
            }
            for (auto const* route : {&storeRoute(), &appendRoute()})
            {
                append(rows, firstReplies(*route, dataOnly()));
                append(
                    rows,
                    {
                        intermediate(*route, "350 Requested file action pending further information."),
                        refused(*route, "452 Requested action not taken: insufficient storage space.", RemoteErrorKind::NoSpace),
                        refused(*route, "532 Need account for storing files.", RemoteErrorKind::PermissionDenied),
                        refused(*route, "551 Requested action aborted: page type unknown.", RemoteErrorKind::Failure),
                        refused(*route, "552 Requested file action aborted: exceeded storage allocation.", RemoteErrorKind::QuotaExceeded),
                        refused(*route, "553 Requested action not taken: file name not allowed.", RemoteErrorKind::PermissionDenied),
                    }
                );
                append(rows, finalReplies(*route, std::string{uploadAnnounced}, WithoutFinalReply::SizeTells));
                append(rows, breakdowns(*route));
            }
            for (auto const* route : {&listRoute(), &machineListRoute()})
            {
                append(rows, firstReplies(*route, dataOnly()));
                append(
                    rows,
                    {
                        intermediate(*route, "350 Requested file action pending further information."),
                        refused(*route, "502 Command not implemented.", RemoteErrorKind::Unsupported),
                    }
                );
                append(rows, finalReplies(*route, std::string{listingAnnounced}, WithoutFinalReply::Fails));
                append(rows, breakdowns(*route));
            }
            append(
                rows,
                {refused(machineListRoute(), "550 /dir: No such file or directory", RemoteErrorKind::NoSuchFile)}
            );
            return rows;
        }

        /**
         * @brief Reads at least @p count bytes of a download, then aborts it and sends NOOP behind the drain.
         *
         * @return The result of the NOOP: the error that ended the connection when the drain failed.
         */
        asio::awaitable<Result> abortAfter(Context context, std::size_t count)
        {
            auto started = co_await Download::start(
                context.session.transfer, DownloadRequest{.path = "/dir/file"}, context.session.options
            );
            if (!started)
                co_return std::unexpected(std::move(started).error());
            auto const download = std::move(started).value();
            std::array<char, 64u> buffer{};
            std::size_t total{0u};
            std::expected<std::size_t, RemoteIo::RemoteError> read = std::size_t{1u};
            while (read && *read != 0u && total < count)
            {
                read = co_await download->readSome(asio::buffer(buffer));
                if (read)
                    total += *read;
            }
            if (!read)
                co_return std::unexpected(std::move(read).error());
            download->abort();
            co_return withoutValue(co_await context.connection->execute(Command{.verb = Verb::Noop}));
        }

        constexpr std::string_view markerReply = "257 \"/dir\" is the current directory";

        Route const& abortRoute()
        {
            // ABOR goes out with PWD behind it, the marker of the drain.
            static const Route route{
                .name = "Abor",
                .before = followedBy(
                    passiveWith("EPSV", DataPlan{}.stallAfter("abc")),
                    [](Script& script)
                    {
                        script.expect("RETR", "/dir/file")
                            .reply("150 Opening BINARY mode data connection for /dir/file (7 bytes).")
                            .beginData();
                    }
                ),
                .command =
                    [](Script& script)
                {
                    script.expect("ABOR", "").expect("PWD", "");
                },
                .success = followedBy(
                    followedBy(replying("426 Connection closed; transfer aborted."), replying("226 Abort successful.")),
                    replying(std::string{markerReply})
                ),
                .afterSuccess = followedBy(expecting("NOOP", ""), replying("200 Command okay.")),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return co_await abortAfter(std::move(context), 3u);
                },
            };
            return route;
        }

        /**
         * @brief The replies @p replies to ABOR, then the marker's, after which the drain ends and the login stays.
         */
        Row drainedBy(std::string outcome, std::vector<std::string> replies)
        {
            auto const& route = abortRoute();
            return Row{
                .name = fmt::format("{}_{}", route.name, outcome),
                .route = &route,
                .outcome =
                    [replies = std::move(replies)](Script& script)
                {
                    for (auto const& reply : replies)
                        script.reply(reply);
                    script.reply(markerReply);
                },
                .then = Then::GoesOn,
            };
        }

        Route const& noopRoute()
        {
            // The pool's NOOP keeps a connection alive, any reply in step will do.
            static const Route route{
                .name = "Noop",
                .command = expecting("NOOP", ""),
                .success = replying("200 Command okay."),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    co_return withoutValue(co_await context.connection->execute(Command{.verb = Verb::Noop}));
                },
            };
            return route;
        }

        Route const& quitRoute()
        {
            // Like the pool: QUIT, then a close whatever came.
            static const Route route{
                .name = "Quit",
                .command = expecting("QUIT", ""),
                .success = replying("221 Goodbye."),
                .drive = [](Context context) -> asio::awaitable<Result>
                {
                    [[maybe_unused]] const auto reply = co_await context.connection->execute(Command{.verb = Verb::Quit});
                    context.connection->close();
                    co_return Result{};
                },
            };
            return route;
        }

        std::vector<Row> sessionRows()
        {
            std::vector<Row> rows{};
            {
                auto const& route = abortRoute();
                append(
                    rows,
                    {
                        succeeds(route),
                        drainedBy("FinalOnly", {"226 Transfer complete."}),
                        drainedBy("FinalFirst", {"226 Abort successful.", "426 Connection closed; transfer aborted."}),
                        drainedBy("OtherCompletion", {"426 Failure writing network stream.", "225 No transfer to ABOR."}),
                        drainedBy("Refused451", {"426 Transfer aborted.", "451 Transfer aborted."}),
                        drainedBy("Refused500", {"500 Syntax error, command unrecognized."}),
                        drainedBy("Refused501", {"501 Syntax error in parameters or arguments."}),
                        drainedBy("Refused502", {"502 Command not implemented."}),
                    }
                );
                append(rows, breakdowns(route));
            }
            {
                auto const& route = noopRoute();
                auto closing = closesTheService(route);
                // The 421 is the reply NOOP waited for, the connection ends behind it.
                closing.kind.reset();
                append(
                    rows,
                    {
                        succeeds(route),
                        completes(route, "250 Requested file action okay."),
                        toleratesPreliminary(route),
                        named(
                            refusedAndIgnored(route, "350 Requested file action pending further information."),
                            "Intermediate"
                        ),
                        refusedAndIgnored(route, "500 Syntax error, command unrecognized."),
                        closing,
                        malformed(route),
                        endsTheStream(route),
                        resets(route),
                        endsInAReply(route),
                        staysSilent(route),
                    }
                );
            }
            {
                // Closing never fails, whatever the server does.
                auto const& route = quitRoute();
                auto quitting = leaving(
                    {
                        succeeds(route),
                        completes(route, "200 Command okay."),
                        toleratesPreliminary(route),
                        refusedAndIgnored(route, "500 Syntax error, command unrecognized."),
                    },
                    Afterwards::Closed
                );
                auto failures = breakdowns(route);
                for (auto& row : failures)
                    row.kind.reset();
                append(rows, std::move(quitting));
                append(rows, std::move(failures));
            }
            return rows;
        }

        std::vector<Row> allRows()
        {
            auto rows = metadataRows();
            append(rows, loginRows());
            append(rows, transferRows());
            append(rows, sessionRows());
            return rows;
        }

        ControlConnectionOptions optionsFor(Row const& row)
        {
            if (row.hurried)
                return ControlConnectionOptions{.connectTimeout = 10s, .commandTimeout = 300ms, .drainTimeout = 300ms};
            return ControlConnectionOptions{.connectTimeout = 10s, .commandTimeout = 10s};
        }

        TransferOptions transferOptionsFor(Row const& row)
        {
            return TransferOptions{
                .data = {.connectTimeout = 5s, .inactivityTimeout = 5s},
                .finalReplyTimeout = row.hurried ? 300ms : 5s,
            };
        }

        Script scriptOf(Row const& row)
        {
            auto const& route = *row.route;
            Script script{};
            script.reply("220 Ready");
            route.before(script);
            route.command(script);
            row.outcome(script);
            switch (row.then)
            {
                case Then::Ends:
                    break;
                case Then::GoesOn:
                    route.afterSuccess(script);
                    break;
                case Then::AsksWhy:
                    route.explanation(script);
                    break;
                case Then::FallsBack:
                    route.fallback(script);
                    break;
            }
            if (row.serverEnds)
                return script;
            if (row.afterwards == Afterwards::Idle)
                script.expect("NOOP").reply("200 Command okay.");
            script.expectDisconnect();
            return script;
        }
    }

    class NonHappyPathMatrix : public ::testing::TestWithParam<Row>
    {
      protected:
        void SetUp() override
        {
            auto started = ScriptedFtpServer::start();
            ASSERT_TRUE(started) << started.error();
            server_ = std::move(started).value();
        }

        void TearDown() override
        {
            if (connection_)
                connection_->close();
            if (!server_->waitUntilDisconnected())
                ADD_FAILURE() << "The server still has a connection open";
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
        }

        template <typename ValueT>
        ValueT run(asio::awaitable<ValueT> awaitable)
        {
            auto future = asio::co_spawn(connection_->strand(), std::move(awaitable), asio::use_future);
            if (future.wait_for(20s) != std::future_status::ready)
                throw std::runtime_error{"The operation did not end"};
            return future.get();
        }

        std::vector<asio::ip::tcp::endpoint> endpoints() const
        {
            return {asio::ip::tcp::endpoint{asio::ip::make_address(server_->address()), server_->port()}};
        }

        Context contextOf(Row const& row) const
        {
            auto dialect = std::make_shared<ServerDialect>();
            row.route->learned(*dialect);
            return Context{
                .connection = connection_,
                .endpoints = endpoints(),
                .session =
                    MetadataSession{
                        .transfer = {.control = connection_, .passiveChoice = std::make_shared<PassiveChoice>()},
                        .dialect = std::move(dialect),
                        .options = transferOptionsFor(row),
                    },
            };
        }

        std::unique_ptr<ScriptedFtpServer> server_{};
        asio::thread_pool pool_{1};
        std::shared_ptr<ControlConnection> connection_{};
    };

    TEST_P(NonHappyPathMatrix, EndsAsTheRowSays)
    {
        auto const& row = GetParam();
        auto const& route = *row.route;
        server_->addConnection(scriptOf(row));
        connection_ = ControlConnection::create(pool_.get_executor(), optionsFor(row));
        if (!route.connectsItself)
        {
            const auto greeting = run(connection_->connect(endpoints()));
            ASSERT_TRUE(greeting) << greeting.error().toString();
        }

        const auto result = run(route.drive(contextOf(row)));

        if (!row.kind)
            ASSERT_TRUE(result) << result.error().toString();
        else
        {
            ASSERT_FALSE(result) << "the operation succeeded";
            EXPECT_EQ(result.error().kind, *row.kind) << result.error().toString();
            EXPECT_EQ(result.error().protocolCode, row.code) << result.error().toString();
            EXPECT_FALSE(result.error().message.empty());
            EXPECT_NE(result.error().toString().find(row.shown), std::string::npos)
                << result.error().toString() << " does not show " << row.shown;
        }
        switch (row.afterwards)
        {
            case Afterwards::Idle:
            {
                EXPECT_EQ(connection_->state(), ControlState::Open);
                const auto noop = run(connection_->execute(Command{.verb = Verb::Noop}));
                ASSERT_TRUE(noop) << noop.error().toString();
                EXPECT_EQ(noop->code, 200);
                break;
            }
            case Afterwards::Dead:
                EXPECT_EQ(connection_->state(), ControlState::Dead);
                break;
            case Afterwards::Closed:
                EXPECT_EQ(connection_->state(), ControlState::Closed);
                break;
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Matrix,
        NonHappyPathMatrix,
        ::testing::ValuesIn(allRows()),
        [](::testing::TestParamInfo<Row> const& info)
        {
            return info.param.name;
        }
    );
}
