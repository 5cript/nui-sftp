#include <ftp/client/control_connection.hpp>
#include <ftp/client/metadata.hpp>
#include <ftp/client/transfer.hpp>
#include <ftp/support/scripted_ftp_server.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <ostream>
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
                        refused(*route, "500 Syntax error, command unrecognized.", RemoteErrorKind::Unsupported),
                        refusedAndAsked(
                            *route, "501 Syntax error in parameters or arguments.", RemoteErrorKind::PermissionDenied
                        ),
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

        std::vector<Row> allRows()
        {
            return metadataRows();
        }

        ControlConnectionOptions optionsFor(Row const& row)
        {
            if (row.hurried)
                return ControlConnectionOptions{.connectTimeout = 10s, .commandTimeout = 300ms};
            return ControlConnectionOptions{.connectTimeout = 10s, .commandTimeout = 10s};
        }

        TransferOptions transferOptions()
        {
            return TransferOptions{
                .data = {.connectTimeout = 5s, .inactivityTimeout = 5s},
                .finalReplyTimeout = 2s,
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

        Context contextOf(Route const& route) const
        {
            auto dialect = std::make_shared<ServerDialect>();
            route.learned(*dialect);
            return Context{
                .connection = connection_,
                .endpoints = endpoints(),
                .session =
                    MetadataSession{
                        .transfer = {.control = connection_, .passiveChoice = std::make_shared<PassiveChoice>()},
                        .dialect = std::move(dialect),
                        .options = transferOptions(),
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
        const auto greeting = run(connection_->connect(endpoints()));
        ASSERT_TRUE(greeting) << greeting.error().toString();

        const auto result = run(route.drive(contextOf(route)));

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
