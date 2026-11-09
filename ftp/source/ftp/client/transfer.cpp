#include <ftp/client/transfer.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/reply_parsers.hpp>
#include <log/log.hpp>
#include "client_support.hpp"

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>

#include <exception>
#include <string_view>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using RemoteIo::RemoteErrorKind;
        using Detail::errorOf;
        using Detail::isCancelled;
        using Detail::useTuple;

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
         * @brief The error of a refused transfer. A refusal that tells of a missing session reuse, behind a full TLS
         *        handshake on the data connection that resumed nothing, is NoSecureConnection and ends the login: its
         *        session would not resume for the next transfer either. A handshake that a refusal cut short proves
         *        nothing.
         */
        RemoteIo::RemoteError
        refusalOf(Reply const& reply, Verb verb, DataConnection const& data, ControlConnection& control)
        {
            auto error = replyError(reply, verb);
            if (!data.secured() || data.resumed() || !tellsOfMissingSessionReuse(reply))
                return error;
            control.close();
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

        /**
         * @brief The first reply of a transfer when it is its final one already.
         */
        std::optional<Reply> finalReplyOf(Reply first)
        {
            if (isPreliminary(first.code))
                return std::nullopt;
            return first;
        }

        /**
         * @brief Writes ABOR into the running transfer of @p command, the control connection takes the replies up to
         *        its marker. Nothing once the transfer or the connection ended.
         */
        void abortTransfer(ControlConnection& control, PendingCommand const& command, std::string_view what)
        {
            if (control.state() != ControlState::Open || command.stage() == PendingCommand::Stage::Completed)
                return;
            if (const auto abortSent = control.send(Command{.verb = Verb::Abort}); !abortSent)
                Log::debug("FTP: ABOR could not be sent for {}", what);
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
        };

        /**
         * @brief Opens the data connection, sends REST when @p restartAt is not 0, then @p transfer, and waits for
         *        its first reply. With TLS the data handshake runs next to that wait, a failed one aborts the transfer.
         */
        asio::awaitable<std::expected<StartedTransfer, RemoteIo::RemoteError>> startTransfer(
            TransferSession const& session,
            TransferOptions const& options,
            std::uint64_t restartAt,
            Command transfer
        )
        {
            // A REST the server took must not stay behind a transfer command that cannot be sent (RFC 3659 5.3).
            if (const auto encoded = transfer.encode(); !encoded)
                co_return std::unexpected(encoded.error());
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
                    // The server may have taken the restart, the next transfer must not start there.
                    if (const auto cleared = control->send(Command{.verb = Verb::Restart, .argument = "0"}); !cleared)
                        Log::debug("FTP: REST 0 could not be sent after a cancelled restart");
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
                data->reset();
                abortTransfer(*control, **command, "a cancelled transfer");
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The transfer was cancelled"));
            }

            if (!first)
            {
                data->close();
                co_return std::unexpected(std::move(first).error());
            }
            if (refused(*first))
                co_return std::unexpected(refusalOf(*first, verb, *data, *control));
            if (!secured)
            {
                // The server went on, the data must not flow unprotected.
                data->reset();
                abortTransfer(*control, **command, "a transfer without TLS");
                co_return std::unexpected(std::move(secured).error());
            }
            co_return StartedTransfer{.data = data, .command = std::move(command).value(), .first = std::move(*first)};
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
        , pauseTimer_{session_.control->strand()}
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
        download->earlyFinalReply_ = finalReplyOf(std::move(started->first));
        // The wait for the first read is a pause as well.
        download->armPauseDrop();
        co_return download;
    }

    asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>> Download::readSome(asio::mutable_buffer buffer)
    {
        if (aborted_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The download was aborted"));
        if (dataEnded_)
            co_return std::size_t{0u};
        reading_ = true;
        const auto read = co_await readData(buffer);
        reading_ = false;
        if (read && *read > 0u)
            armPauseDrop();
        co_return read;
    }

    asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>> Download::readData(asio::mutable_buffer buffer)
    {
        if (dropped_)
        {
            if (auto resumed = co_await resume(); !resumed)
                co_return std::unexpected(std::move(resumed).error());
        }
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
        pauseTimer_.cancel();
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
            co_return std::unexpected(refusalOf(*reply, Verb::Retrieve, *data_, *session_.control));
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

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> Download::resume()
    {
        auto started = co_await startTransfer(
            session_, options_, request_.offset + bytes_, Command{.verb = Verb::Retrieve, .argument = request_.path}
        );
        if (!started)
        {
            aborted_ = true;
            co_return std::unexpected(std::move(started).error());
        }
        data_ = started->data;
        retrieve_ = started->command;
        dropped_ = false;
        earlyFinalReply_ = finalReplyOf(std::move(started->first));
        Log::debug("FTP: Continued a paused download with REST");
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    void Download::armPauseDrop()
    {
        lastRead_ = std::chrono::steady_clock::now();
        if (!options_.pauseDropAfter || !session_.restartStream || pauseArmed_)
            return;
        waitForPause(*options_.pauseDropAfter);
    }

    void Download::waitForPause(std::chrono::steady_clock::duration duration)
    {
        pauseArmed_ = true;
        pauseTimer_.expires_after(duration);
        pauseTimer_.async_wait(
            [weak = weak_from_this()](boost::system::error_code error)
            {
                const auto self = weak.lock();
                if (!self)
                    return;
                self->pauseArmed_ = false;
                if (!error)
                    self->takePause();
            }
        );
    }

    void Download::takePause()
    {
        // A read under way is no pause, the data it brings starts the timer again.
        if (reading_ || dataEnded_ || aborted_ || dropped_)
            return;
        const auto idle = std::chrono::steady_clock::now() - lastRead_;
        if (idle < *options_.pauseDropAfter)
        {
            waitForPause(*options_.pauseDropAfter - idle);
            return;
        }
        dropForPause();
    }

    void Download::dropForPause()
    {
        Log::debug("FTP: Dropped the data connection of a paused download");
        dropped_ = true;
        data_->close();
        abortTransfer(*session_.control, *retrieve_, "a paused download");
    }

    void Download::abort()
    {
        if (aborted_)
            return;
        aborted_ = true;
        pauseTimer_.cancel();
        data_->close();
        abortTransfer(*session_.control, *retrieve_, "a failed download");
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

    Upload::~Upload()
    {
        if (aborted_ || dataEnded_)
            return;
        // A FIN could make the server take the cut file for a complete one. The last owner may let go off the strand.
        asio::post(
            session_.control->strand(),
            [data = std::move(data_)]()
            {
                data->reset();
            }
        );
    }

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
        upload->earlyFinalReply_ = finalReplyOf(std::move(started->first));
        co_return upload;
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> Upload::write(asio::const_buffer buffer)
    {
        if (aborted_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The upload was aborted"));
        const auto written = co_await data_->writeAll(buffer);
        if (!written)
        {
            // A cancelled coroutine would throw at its next co_await.
            if (isCancelled(co_await asio::this_coro::cancellation_state))
            {
                abort();
                co_return std::unexpected(written.error());
            }
            const auto answer = co_await answerToFailedData();
            co_return std::unexpected(failureOf(answer, written.error()));
        }
        bytes_ += buffer.size();
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    asio::awaitable<std::expected<UploadResult, RemoteIo::RemoteError>> Upload::finish()
    {
        if (aborted_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The upload was aborted"));
        const auto ended = co_await data_->finishUpload();
        if (!ended)
        {
            if (isCancelled(co_await asio::this_coro::cancellation_state))
            {
                abort();
                co_return std::unexpected(ended.error());
            }
            // Every byte was written: a server that confirms the upload took it, however its side of the data ended.
            const auto answer = co_await answerToFailedData();
            if (!answer || !succeeded(*answer))
                co_return std::unexpected(failureOf(answer, ended.error()));
            dataEnded_ = true;
            co_return UploadResult{.bytes = bytes_, .finalReply = *answer};
        }
        dataEnded_ = true;

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
                co_return std::unexpected(errorOf(
                    RemoteErrorKind::TimedOut,
                    "The server did not confirm the upload in time, only the file's size tells whether it is complete"
                ));
            co_return std::unexpected(reply.error());
        }
        if (!succeeded(*reply))
            co_return std::unexpected(refusalOf(*reply, verb(), *data_, *session_.control));
        co_return UploadResult{.bytes = bytes_, .finalReply = std::move(*reply)};
    }

    asio::awaitable<std::optional<Reply>> Upload::answerToFailedData()
    {
        // A server that refused the rest, for a full disk say, may close the data connection before its reply comes.
        const auto [exception, reply] = co_await asio::co_spawn(
            session_.control->strand(), store_->finalReply(), asio::cancel_after(options_.refusalWait, useTuple)
        );
        if (exception || !reply)
            co_return std::nullopt;
        co_return *reply;
    }

    RemoteIo::RemoteError Upload::failureOf(std::optional<Reply> const& answer, RemoteIo::RemoteError dataError)
    {
        if (answer && refused(*answer))
        {
            aborted_ = true;
            data_->reset();
            return refusalOf(*answer, verb(), *data_, *session_.control);
        }
        abort();
        return dataError;
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
        abortTransfer(*session_.control, *store_, "an aborted upload");
    }
}
