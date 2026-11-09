#include <ftp/client/transfer.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/reply_parsers.hpp>
#include <log/log.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>

#include <exception>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using RemoteIo::RemoteErrorKind;

        RemoteIo::RemoteError errorOf(RemoteErrorKind kind, std::string message)
        {
            return RemoteIo::RemoteError{
                .kind = kind,
                .protocol = RemoteIo::Protocol::Ftp,
                .message = std::move(message),
            };
        }

        bool isCancelled(asio::cancellation_state const& state)
        {
            return state.cancelled() != asio::cancellation_type::none;
        }

        bool succeeded(Reply const& reply)
        {
            return classifyReply(reply.code) == ReplyClass::PositiveCompletion;
        }

        bool refused(Reply const& reply)
        {
            const auto replyClass = classifyReply(reply.code);
            return replyClass == ReplyClass::TransientNegative || replyClass == ReplyClass::PermanentNegative;
        }

        /**
         * @brief A refused transfer after a TLS data connection that resumed nothing: vsftpd answers 522, proftpd 425
         *        and others 450 when they require the control connection's session.
         */
        bool refusedForSessionReuse(Reply const& reply, DataConnection const& data)
        {
            return data.secured() && !data.resumed() && (reply.code == 522 || reply.code == 425 || reply.code == 450);
        }

        RemoteIo::RemoteError sessionReuseRequired(Reply const& reply, Verb verb)
        {
            auto error = replyError(reply, verb);
            error.kind = RemoteErrorKind::NoSecureConnection;
            error.message = fmt::format(
                "The server requires data connections to resume the control connection's TLS session: {}", error.message
            );
            return error;
        }

        asio::awaitable<std::expected<void, RemoteIo::RemoteError>> secureData(
            std::shared_ptr<DataConnection> data,
            std::shared_ptr<ControlConnection> control,
            TlsConnectionSettings tls
        )
        {
            auto const& trusted = control->trustedCertificate();
            if (!trusted)
                co_return std::unexpected(errorOf(
                    RemoteErrorKind::NoSecureConnection, "The control connection has no TLS for its data to share"
                ));
            const auto controlCertificate = trusted->certificate.certificateSha256;
            co_return co_await data->secure(std::move(tls), control->sessionForData(), controlCertificate);
        }

        /**
         * @brief The first reply to a transfer command. A refusal closes the data side, a handshake under way included.
         */
        asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>>
        firstReply(std::shared_ptr<PendingCommand> pending, std::shared_ptr<DataConnection> data)
        {
            auto reply = co_await pending->nextReply();
            if (!reply || refused(*reply))
                data->close();
            co_return reply;
        }
    }

    namespace
    {
        /**
         * @brief A transfer command that runs: its data connection, the command, its first reply.
         */
        struct StartedTransfer
        {
            std::shared_ptr<DataConnection> data{};
            std::shared_ptr<PendingCommand> command{};
            Reply first{};
            /** @brief The data handshake failed while the server went on, the caller aborts. */
            std::optional<RemoteIo::RemoteError> unsecured{};
        };

        /**
         * @brief Opens the data connection, sends REST when @p restartAt is not 0, then @p transfer, and waits for
         *        its first reply. With TLS the data handshake runs next to that wait.
         */
        asio::awaitable<std::expected<StartedTransfer, RemoteIo::RemoteError>> startTransfer(
            TransferSession const& session,
            TransferOptions const& options,
            std::uint64_t restartAt,
            Command transfer
        )
        {
            auto const control = session.control;
            const auto verb = transfer.verb;
            auto opened = co_await DataConnection::open(control, options.data, session.passiveChoice);
            if (!opened)
                co_return std::unexpected(std::move(opened).error());
            auto const data = std::move(opened).value();

            if (restartAt > 0u)
            {
                const auto restart =
                    co_await control->execute(Command{.verb = Verb::Restart, .argument = fmt::format("{}", restartAt)});
                if (isCancelled(co_await asio::this_coro::cancellation_state))
                {
                    data->close();
                    co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The transfer was cancelled"));
                }
                if (!restart || restart->code != 350)
                {
                    data->close();
                    co_return std::unexpected(restart ? replyError(*restart, Verb::Restart) : restart.error());
                }
            }

            auto command = control->send(std::move(transfer));
            if (!command)
            {
                data->close();
                co_return std::unexpected(std::move(command).error());
            }

            std::expected<Reply, RemoteIo::RemoteError> first = std::unexpected(RemoteIo::RemoteError{});
            std::expected<void, RemoteIo::RemoteError> secured{};
            if (session.tls)
            {
                auto [order, secureException, secureResult, replyException, replyResult] =
                    co_await asio::experimental::make_parallel_group(
                        asio::co_spawn(control->strand(), secureData(data, control, *session.tls), asio::deferred),
                        asio::co_spawn(control->strand(), firstReply(*command, data), asio::deferred)
                    )
                        .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
                if (secureException)
                    std::rethrow_exception(secureException);
                if (replyException)
                    std::rethrow_exception(replyException);
                secured = std::move(secureResult);
                first = std::move(replyResult);
            }
            else
                first = co_await firstReply(*command, data);
            if (isCancelled(co_await asio::this_coro::cancellation_state))
            {
                data->close();
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The transfer was cancelled"));
            }

            if (!first)
            {
                data->close();
                co_return std::unexpected(std::move(first).error());
            }
            if (refused(*first))
            {
                if (refusedForSessionReuse(*first, *data))
                {
                    control->close();
                    co_return std::unexpected(sessionReuseRequired(*first, verb));
                }
                co_return std::unexpected(replyError(*first, verb));
            }
            StartedTransfer started{.data = data, .command = std::move(command).value(), .first = std::move(*first)};
            if (!secured)
                started.unsecured = std::move(secured).error();
            co_return started;
        }
    }

    Download::Download(
        Private,
        TransferSession session,
        DownloadRequest request,
        TransferOptions options,
        std::shared_ptr<DataConnection> data,
        std::shared_ptr<PendingCommand> retrieve
    )
        : session_{std::move(session)}
        , request_{std::move(request)}
        , options_{std::move(options)}
        , data_{std::move(data)}
        , retrieve_{std::move(retrieve)}
    {}

    Download::~Download() = default;

    asio::awaitable<std::expected<std::shared_ptr<Download>, RemoteIo::RemoteError>>
    Download::start(TransferSession session, DownloadRequest request, TransferOptions options)
    {
        auto started = co_await startTransfer(
            session, options, request.offset, Command{.verb = Verb::Retrieve, .argument = request.path}
        );
        if (!started)
            co_return std::unexpected(std::move(started).error());
        auto download = std::make_shared<Download>(
            Private{}, std::move(session), std::move(request), std::move(options), started->data, started->command
        );
        if (download->request_.offset == 0u)
            download->announced_ = parseAnnouncedSize(started->first);
        if (!isPreliminary(started->first.code))
            download->earlyFinalReply_ = std::move(started->first);
        if (started->unsecured)
        {
            download->abort();
            co_return std::unexpected(std::move(*started->unsecured));
        }
        co_return download;
    }

    asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>> Download::readSome(asio::mutable_buffer buffer)
    {
        if (aborted_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The download was aborted"));
        if (dataEnded_)
            co_return std::size_t{0u};
        const auto read = co_await data_->readSome(buffer);
        if (!read)
        {
            abort();
            co_return std::unexpected(read.error());
        }
        if (*read > 0u)
        {
            bytes_ += *read;
            co_return *read;
        }
        dataEnded_ = true;
        co_await data_->finishDownload();
        co_return std::size_t{0u};
    }

    asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>> Download::finish()
    {
        if (aborted_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The download was aborted"));
        if (!dataEnded_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "The download's data did not end yet"));

        std::expected<Reply, RemoteIo::RemoteError> reply = std::unexpected(RemoteIo::RemoteError{});
        if (earlyFinalReply_)
            reply = *earlyFinalReply_;
        else
        {
            session_.control->limitFinalReply(*retrieve_, options_.finalReplyTimeout);
            reply = co_await retrieve_->finalReply();
            if (isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The download was cancelled"));
        }

        if (!reply)
        {
            // Its deadline ended the control connection. All the data that should come is proof enough.
            const auto expected = expectedBytes();
            if (reply.error().kind == RemoteErrorKind::TimedOut && expected == bytes_ &&
                !data_->endedWithoutCloseNotify())
            {
                Log::debug("FTP: Accepted a download without its final reply, its size matched");
                co_return TransferResult{.bytes = bytes_};
            }
            co_return std::unexpected(reply.error());
        }
        if (!succeeded(*reply))
        {
            if (refusedForSessionReuse(*reply, *data_))
            {
                session_.control->close();
                co_return std::unexpected(sessionReuseRequired(*reply, Verb::Retrieve));
            }
            co_return std::unexpected(replyError(*reply, Verb::Retrieve));
        }
        co_return co_await checkSize(std::move(*reply));
    }

    std::optional<std::uint64_t> Download::announcedSize() const
    {
        return announced_;
    }

    std::optional<std::uint64_t> Download::expectedBytes() const
    {
        if (request_.expectedSize)
        {
            if (*request_.expectedSize < request_.offset)
                return std::nullopt;
            return *request_.expectedSize - request_.offset;
        }
        return announced_;
    }

    asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>> Download::checkSize(Reply finalReply)
    {
        const auto expected = expectedBytes();
        if (expected == bytes_ || (!expected && !data_->endedWithoutCloseNotify()))
            co_return TransferResult{.bytes = bytes_, .finalReply = std::move(finalReply)};

        // Fewer or more bytes than expected, or TLS that ended without close_notify: the server's size decides now.
        const auto reply = co_await session_.control->execute(Command{.verb = Verb::Size, .argument = request_.path});
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The download was cancelled"));
        const auto size = reply.and_then(
            [](Reply const& sizeReply)
            {
                return parseSizeReply(sizeReply);
            }
        );
        if (size && *size == request_.offset + bytes_)
            co_return TransferResult{.bytes = bytes_, .finalReply = std::move(finalReply)};
        auto truncated = errorOf(
            RemoteErrorKind::Truncated,
            size ? fmt::format("The download ended after {} of {} bytes", request_.offset + bytes_, *size)
                 : fmt::format(
                       "The download ended after {} bytes, the server's size is unknown", request_.offset + bytes_
                   )
        );
        truncated.protocolCode = finalReply.code;
        co_return std::unexpected(std::move(truncated));
    }

    void Download::abort()
    {
        if (aborted_)
            return;
        aborted_ = true;
        data_->close();
        // Writes ABOR into the running transfer, the control connection takes the replies up to its marker.
        if (session_.control->state() == ControlState::Open && retrieve_->stage() != PendingCommand::Stage::Completed)
        {
            if (const auto abortSent = session_.control->send(Command{.verb = Verb::Abort}); !abortSent)
                Log::debug("FTP: ABOR could not be sent for a failed download");
        }
    }

    Upload::Upload(
        Private,
        TransferSession session,
        UploadRequest request,
        TransferOptions options,
        std::shared_ptr<DataConnection> data,
        std::shared_ptr<PendingCommand> store
    )
        : session_{std::move(session)}
        , request_{std::move(request)}
        , options_{std::move(options)}
        , data_{std::move(data)}
        , store_{std::move(store)}
    {}

    Upload::~Upload() = default;

    asio::awaitable<std::expected<std::shared_ptr<Upload>, RemoteIo::RemoteError>>
    Upload::start(TransferSession session, UploadRequest request, TransferOptions options)
    {
        const bool appending = request.mode == UploadMode::Append;
        const auto restartAt = appending ? std::uint64_t{0u} : request.offset;
        auto started = co_await startTransfer(
            session,
            options,
            restartAt,
            Command{.verb = appending ? Verb::Append : Verb::Store, .argument = request.path}
        );
        if (!started)
            co_return std::unexpected(std::move(started).error());
        auto upload = std::make_shared<Upload>(
            Private{}, std::move(session), std::move(request), std::move(options), started->data, started->command
        );
        if (!isPreliminary(started->first.code))
            upload->earlyFinalReply_ = std::move(started->first);
        if (started->unsecured)
        {
            upload->abort();
            co_return std::unexpected(std::move(*started->unsecured));
        }
        co_return upload;
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> Upload::write(asio::const_buffer buffer)
    {
        if (aborted_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The upload was aborted"));
        const auto written = co_await data_->writeAll(buffer);
        if (!written)
            co_return std::unexpected(co_await failure(written.error()));
        bytes_ += buffer.size();
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    asio::awaitable<std::expected<UploadResult, RemoteIo::RemoteError>> Upload::finish()
    {
        if (aborted_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The upload was aborted"));
        const auto ended = co_await data_->finishUpload();
        if (!ended)
            co_return std::unexpected(co_await failure(ended.error()));

        std::expected<Reply, RemoteIo::RemoteError> reply = std::unexpected(RemoteIo::RemoteError{});
        if (earlyFinalReply_)
            reply = *earlyFinalReply_;
        else
        {
            session_.control->limitFinalReply(*store_, options_.finalReplyTimeout);
            reply = co_await store_->finalReply();
            if (isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The upload was cancelled"));
        }
        if (!reply)
        {
            // Its deadline ended the control connection, SIZE on another one has to tell.
            if (reply.error().kind == RemoteErrorKind::TimedOut)
                co_return UploadResult{.bytes = bytes_};
            co_return std::unexpected(reply.error());
        }
        if (!succeeded(*reply))
        {
            if (refusedForSessionReuse(*reply, *data_))
            {
                session_.control->close();
                co_return std::unexpected(sessionReuseRequired(*reply, verb()));
            }
            co_return std::unexpected(replyError(*reply, verb()));
        }
        co_return UploadResult{.bytes = bytes_, .finalReply = std::move(*reply)};
    }

    asio::awaitable<RemoteIo::RemoteError> Upload::failure(RemoteIo::RemoteError dataError)
    {
        // A server that refused the rest, for a full disk say, closed the data connection behind its reply.
        if (store_->stage() == PendingCommand::Stage::Completed)
        {
            const auto reply = co_await store_->finalReply();
            if (reply && refused(*reply))
            {
                aborted_ = true;
                data_->reset();
                co_return replyError(*reply, verb());
            }
        }
        abort();
        co_return dataError;
    }

    Verb Upload::verb() const
    {
        if (request_.mode == UploadMode::Append)
            return Verb::Append;
        return Verb::Store;
    }

    void Upload::abort()
    {
        if (aborted_)
            return;
        aborted_ = true;
        data_->reset();
        if (session_.control->state() == ControlState::Open && store_->stage() != PendingCommand::Stage::Completed)
        {
            if (const auto abortSent = session_.control->send(Command{.verb = Verb::Abort}); !abortSent)
                Log::debug("FTP: ABOR could not be sent for a failed upload");
        }
    }
}
