#include <ftp/client/transfer.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/reply_parsers.hpp>
#include <log/log.hpp>
#include "client_support.hpp"

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>

#include <cassert>
#include <exception>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using RemoteIo::RemoteErrorKind;
        using Detail::errorOf;
        using Detail::isCancelled;
        using Detail::refused;
        using Detail::succeeded;
        using Detail::useTuple;

        /** @brief The bytes of listing data read at once. */
        constexpr std::size_t listingReadSize{64u * 1024u};

        RemoteIo::RemoteError cancelled()
        {
            return errorOf(RemoteErrorKind::Cancelled, "The transfer was cancelled");
        }

        /**
         * @brief What a transfer answers once it was aborted.
         */
        RemoteIo::RemoteError aborted()
        {
            return errorOf(RemoteErrorKind::Cancelled, "The transfer was aborted");
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
         * @brief A 3yz asks for more than a transfer command can give (RFC 959 5.4), the transfer does not start.
         */
        bool asksForMore(Reply const& reply)
        {
            return classifyReply(reply.code) == ReplyClass::PositiveIntermediate;
        }

        /**
         * @brief The first reply to a transfer command. A refusal, a 3yz or a wait that failed closes the data side, a
         *        handshake under way included. A cancel does not: it ends the handshake by itself, and
         *        startTransfer() ends the data.
         */
        asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>>
        firstReply(std::shared_ptr<PendingCommand> pending, std::shared_ptr<DataConnection> data)
        {
            auto reply = co_await pending->nextReply();
            if (isCancelled(co_await asio::this_coro::cancellation_state))
                co_return reply;
            if (!reply || refused(*reply) || asksForMore(*reply))
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
         * @brief What ends the data of an upload that does not finish.
         */
        struct UploadEnd
        {
            std::shared_ptr<DataConnection> data{};
            std::shared_ptr<ControlConnection> control{};
            std::shared_ptr<PendingCommand> store{};
            std::chrono::milliseconds finalReplyTimeout{};
        };

        /**
         * @brief Ends @p data so that the server takes the empty upload, then limits the final reply of @p store to
         *        @p finalReplyTimeout: no ABOR follows a clean end that would limit it.
         */
        asio::awaitable<void> endedCleanly(
            std::shared_ptr<DataConnection> data,
            std::weak_ptr<ControlConnection> control,
            std::shared_ptr<PendingCommand> store,
            std::chrono::milliseconds finalReplyTimeout
        )
        {
            std::ignore = co_await data->finishUpload();
            if (const auto locked = control.lock())
                locked->limitFinalReply(*store, finalReplyTimeout);
        }

        /**
         * @brief Resets the data of @p end on the strand it is on, or ends it cleanly where the server may still be in
         *        its TLS handshake.
         *
         * @return Whether it was reset, which leaves the server a transfer to abort.
         */
        bool endUploadData(UploadEnd end)
        {
            if (!end.data->serverMayBeInHandshake())
            {
                end.data->reset();
                return true;
            }
            const auto strand = end.control->strand();
            asio::co_spawn(
                strand,
                endedCleanly(std::move(end.data), end.control, std::move(end.store), end.finalReplyTimeout),
                asio::detached
            );
            return false;
        }

        /**
         * @brief Ends the data of a transfer that was cancelled while it started, where the server may still be in
         *        its TLS handshake without a reset: an upload as endUploadData() does, anything else once the server
         *        answered on it.
         *
         * @return Whether the server is left a transfer to abort.
         */
        bool endStartedData(Verb verb, UploadEnd end)
        {
            if (verb == Verb::Store || verb == Verb::Append)
                return endUploadData(std::move(end));
            end.data->closeOnceAnswered();
            return true;
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

        /**
         * @brief The final reply of @p command once its data ended: @p early when it came before the end, else the
         *        one that comes within @p timeout.
         *
         * @return Cancelled for a cancel that came with the reply, the caller must await nothing more then.
         */
        asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> finalReplyBehindData(
            ControlConnection& control,
            PendingCommand& command,
            std::optional<Reply> early,
            std::chrono::milliseconds timeout
        )
        {
            if (early)
                co_return std::move(*early);
            control.limitFinalReply(command, timeout);
            auto reply = co_await command.finalReply();
            if (isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(cancelled());
            co_return reply;
        }

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
                    co_return std::unexpected(cancelled());
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
                const UploadEnd end{
                    .data = data,
                    .control = control,
                    .store = *command,
                    .finalReplyTimeout = options.finalReplyTimeout,
                };
                if (endStartedData(verb, end))
                    abortTransfer(*control, **command, "a cancelled transfer");
                co_return std::unexpected(cancelled());
            }

            if (!first)
            {
                data->close();
                co_return std::unexpected(std::move(first).error());
            }
            if (refused(*first))
                co_return std::unexpected(refusalOf(*first, verb, *data, *control));
            if (asksForMore(*first))
                co_return std::unexpected(replyError(*first, verb));
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
            co_return std::unexpected(aborted());
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
            co_return std::unexpected(aborted());
        if (!dataEnded_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "The download's data did not end yet"));

        auto reply =
            co_await finalReplyBehindData(*session_.control, *retrieve_, earlyFinalReply_, options_.finalReplyTimeout);
        // An abort ends the download as cancelled, whatever the server answers behind it.
        if (aborted_)
            co_return std::unexpected(aborted());
        if (!reply)
        {
            // Its deadline ended the control connection. All the data that should come is proof enough.
            const auto expected = expectedBytes();
            if (reply.error().kind == RemoteErrorKind::TimedOut && expected == bytes_ &&
                !data_->endedWithoutCloseNotify())
            {
                Log::debug("FTP: Accepted a download without its final reply, its size matched");
                co_return TransferResult{.bytes = bytes_, .resumedSession = data_->resumed()};
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
        // Only a download that started at 0 reads the announcement, which names the whole file then.
        const auto whole = request_.expectedSize ? request_.expectedSize : announced_;
        if (!whole || *whole < request_.offset)
            return std::nullopt;
        return *whole - request_.offset;
    }

    asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>> Download::checkSize(Reply finalReply)
    {
        const auto expected = expectedBytes();
        if (expected == bytes_ || (!expected && !data_->endedWithoutCloseNotify()))
            co_return TransferResult{
                .bytes = bytes_, .finalReply = std::move(finalReply), .resumedSession = data_->resumed()
            };

        // Fewer or more bytes than expected, or TLS that ended without close_notify: the server's size decides now.
        const auto reply = co_await session_.control->execute(Command{.verb = Verb::Size, .argument = request_.path});
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return std::unexpected(cancelled());
        if (aborted_)
            co_return std::unexpected(aborted());
        const auto size = reply.and_then(
            [](Reply const& sizeReply)
            {
                return parseSizeReply(sizeReply);
            }
        );
        if (size && *size == request_.offset + bytes_)
            co_return TransferResult{
                .bytes = bytes_, .finalReply = std::move(finalReply), .resumedSession = data_->resumed()
            };
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
        Log::debug("FTP: Started a dropped download again at its position");
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
        drop("a paused download");
    }

    void Download::drop(std::string_view why)
    {
        dropped_ = true;
        data_->closeOnceAnswered();
        abortTransfer(*session_.control, *retrieve_, why);
    }

    std::expected<void, RemoteIo::RemoteError> Download::seek(std::uint64_t position)
    {
        assert(!reading_ && "a seek during a read would end the read as a failure");
        if (aborted_)
            return std::unexpected(aborted());
        if (position == request_.offset + bytes_)
            return std::expected<void, RemoteIo::RemoteError>{};
        if (!dropped_)
            drop("a seek");
        request_.offset = position;
        bytes_ = 0u;
        dataEnded_ = false;
        earlyFinalReply_.reset();
        return std::expected<void, RemoteIo::RemoteError>{};
    }

    void Download::abort()
    {
        if (aborted_)
            return;
        aborted_ = true;
        pauseTimer_.cancel();
        data_->closeOnceAnswered();
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
            [end =
                    UploadEnd{
                        .data = std::move(data_),
                        .control = session_.control,
                        .store = std::move(store_),
                        .finalReplyTimeout = options_.finalReplyTimeout,
                    }]() mutable
            {
                endUploadData(std::move(end));
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
            co_return std::unexpected(aborted());
        writing_ = true;
        const auto written = co_await data_->writeAll(buffer);
        writing_ = false;
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
            co_return std::unexpected(aborted());
        ending_ = true;
        const auto ended = co_await data_->finishUpload();
        // An abort meanwhile reset the data, a 226 behind it would not complete the upload.
        if (aborted_)
            co_return std::unexpected(aborted());
        if (!ended)
        {
            if (isCancelled(co_await asio::this_coro::cancellation_state))
            {
                abort();
                co_return std::unexpected(ended.error());
            }
            // Every byte was written: a server that confirms the upload took it, however its side of the data ended.
            const auto answer = co_await answerToFailedData();
            if (aborted_)
                co_return std::unexpected(aborted());
            if (!answer || !succeeded(*answer))
                co_return std::unexpected(failureOf(answer, ended.error()));
            dataEnded_ = true;
            co_return UploadResult{.bytes = bytes_, .finalReply = *answer, .resumedSession = data_->resumed()};
        }
        dataEnded_ = true;

        auto reply =
            co_await finalReplyBehindData(*session_.control, *store_, earlyFinalReply_, options_.finalReplyTimeout);
        // The server has the whole file by now, the upload still ends as the abort said.
        if (aborted_)
            co_return std::unexpected(aborted());
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
        co_return UploadResult{.bytes = bytes_, .finalReply = std::move(*reply), .resumedSession = data_->resumed()};
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
        // An ABOR beside a clean end can make proftpd answer with garbage in its 426, the server answers it as any end.
        if (endData())
            abortTransfer(*session_.control, *store_, "an aborted upload");
    }

    bool Upload::endData()
    {
        if (writing_ || ending_)
        {
            data_->reset();
            return true;
        }
        return endUploadData(
            UploadEnd{
                .data = data_,
                .control = session_.control,
                .store = store_,
                .finalReplyTimeout = options_.finalReplyTimeout,
            }
        );
    }

    asio::awaitable<std::expected<Listing, RemoteIo::RemoteError>>
    fetchListing(TransferSession session, Command command, ListingParser parser, TransferOptions options)
    {
        assert(
            (command.verb == Verb::List || command.verb == Verb::MachineListDirectory ||
                command.verb == Verb::NameList) &&
            "fetchListing() sends LIST, MLSD or NLST"
        );
        const auto verb = command.verb;
        auto started = co_await startTransfer(session, options, 0u, std::move(command));
        if (!started)
            co_return std::unexpected(std::move(started).error());
        auto const& control = session.control;
        auto const& data = started->data;
        auto early = finalReplyOf(std::move(started->first));

        std::vector<char> buffer(listingReadSize);
        auto ended = false;
        while (!ended)
        {
            const auto read = co_await data->readSome(asio::buffer(buffer));
            if (!read)
            {
                data->closeOnceAnswered();
                abortTransfer(*control, *started->command, "a failed listing");
                co_return std::unexpected(read.error());
            }
            ended = *read == 0u;
            parser.feed(std::string_view{buffer.data(), *read});
        }
        co_await data->finishDownload();

        auto reply =
            co_await finalReplyBehindData(*control, *started->command, std::move(early), options.finalReplyTimeout);
        if (!reply)
            co_return std::unexpected(std::move(reply).error());
        if (!succeeded(*reply))
            co_return std::unexpected(refusalOf(*reply, verb, *data, *control));
        // A listing has no size that could show the cut data complete.
        if (data->endedWithoutCloseNotify())
            co_return std::unexpected(
                errorOf(RemoteErrorKind::Truncated, "The TLS data of the listing ended without close_notify")
            );
        co_return parser.finish();
    }
}
