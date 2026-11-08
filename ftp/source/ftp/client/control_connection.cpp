#include <ftp/client/control_connection.hpp>
#include <ftp/protocol/protocol_error.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <log/log.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Clock = std::chrono::steady_clock;
        using RemoteIo::RemoteErrorKind;

        constexpr auto useTuple = asio::as_tuple(asio::use_awaitable);

        /** @brief The reply of PWD, the marker behind ABOR. No reply to a transfer or to ABOR has this code. */
        constexpr int markerReplyCode = 257;

        RemoteIo::RemoteError errorOf(RemoteErrorKind kind, std::string message)
        {
            return RemoteIo::RemoteError{
                .kind = kind,
                .protocol = RemoteIo::Protocol::Ftp,
                .message = std::move(message),
            };
        }

        RemoteIo::RemoteError transportError(std::string_view operation, boost::system::error_code error)
        {
            return errorOf(
                RemoteErrorKind::ConnectionLost,
                fmt::format("{} the control connection failed: {}", operation, error.message())
            );
        }

        /**
         * @brief Why no endpoint accepted: TimedOut once @p deadline passed, before it NoConnection with @p lastError.
         */
        RemoteIo::RemoteError
        connectError(boost::system::error_code lastError, Clock::time_point deadline, std::chrono::milliseconds timeout)
        {
            if (Clock::now() >= deadline)
                return errorOf(RemoteErrorKind::TimedOut, fmt::format("No connection within {} ms", timeout.count()));
            return errorOf(RemoteErrorKind::NoConnection, fmt::format("Could not connect: {}", lastError.message()));
        }

        /**
         * @brief A command whose final reply follows its whole data transfer.
         */
        bool isTransfer(Verb verb)
        {
            switch (verb)
            {
                case Verb::Retrieve:
                case Verb::Store:
                case Verb::Append:
                case Verb::List:
                case Verb::NameList:
                case Verb::MachineListDirectory:
                    return true;
                default:
                    return false;
            }
        }

        bool isTransferCommand(std::shared_ptr<PendingCommand> const& pending)
        {
            const auto verb = pending->verb();
            return verb && isTransfer(*verb);
        }

        /**
         * @brief The greeting, which may come after a 120, or a transfer.
         */
        bool takesPreliminaryReplies(PendingCommand const& pending)
        {
            const auto verb = pending.verb();
            return !verb || isTransfer(*verb);
        }

        std::string nameOf(PendingCommand const& pending)
        {
            const auto verb = pending.verb();
            if (!verb)
                return "the greeting";
            return std::string{wireName(*verb).value_or("an unknown verb")};
        }

        bool acceptsAuthentication(int code)
        {
            return code == 234 || code == 334;
        }

        RemoteIo::RemoteError refusedAuthentication(Reply const& reply)
        {
            auto error = replyError(reply, Verb::Authenticate);
            error.message = fmt::format("The server refused AUTH TLS: {}", error.message);
            return error;
        }

        bool isCancelled(asio::cancellation_state const& state)
        {
            return state.cancelled() != asio::cancellation_type::none;
        }
    }

    /**
     * @brief The socket and, once there is one, the TLS stream over it. Every operation in flight holds it next to the
     *        connection, so it outlives them. Used on the strand of the connection only.
     */
    class ControlConnection::Transport
    {
      public:
        explicit Transport(asio::strand<asio::any_io_executor> const& strand)
            : socket_{strand}
        {}

        asio::ip::tcp::socket& socket()
        {
            return socket_;
        }

        bool secured() const
        {
            return tls_.has_value();
        }

        void secure(SslHandle handle)
        {
            // The stream takes the handle over.
            tls_.emplace(socket_, handle.release());
        }

        asio::ssl::stream<asio::ip::tcp::socket&>& tls()
        {
            return *tls_;
        }

        SSL* tlsHandle()
        {
            return tls_ ? tls_->native_handle() : nullptr;
        }

        template <typename HandlerT>
        void readSome(asio::mutable_buffer buffer, HandlerT&& handler)
        {
            if (tls_)
                tls_->async_read_some(buffer, std::forward<HandlerT>(handler));
            else
                socket_.async_read_some(buffer, std::forward<HandlerT>(handler));
        }

        template <typename BuffersT, typename HandlerT>
        void write(BuffersT const& buffers, HandlerT&& handler)
        {
            if (tls_)
                asio::async_write(*tls_, buffers, std::forward<HandlerT>(handler));
            else
                asio::async_write(socket_, buffers, std::forward<HandlerT>(handler));
        }

        void close()
        {
            boost::system::error_code ignored{};
            socket_.close(ignored);
        }

        /**
         * @brief Sends close_notify without waiting for the server's, then closes, within @p timeout.
         */
        static void closeAfterNotify(std::shared_ptr<Transport> transport, std::chrono::milliseconds timeout)
        {
            // The server's close_notify counts as received, the shutdown ends once ours is out.
            SSL_set_shutdown(transport->tlsHandle(), SSL_RECEIVED_SHUTDOWN);
            auto& stream = *transport->tls_;
            stream.async_shutdown(
                asio::cancel_after(
                    timeout,
                    [transport = std::move(transport)](boost::system::error_code)
                    {
                        transport->close();
                    }
                )
            );
        }

      private:
        asio::ip::tcp::socket socket_;
        std::optional<asio::ssl::stream<asio::ip::tcp::socket&>> tls_{};
    };

    std::shared_ptr<ControlConnection>
    ControlConnection::create(asio::any_io_executor executor, ControlConnectionOptions options)
    {
        return std::make_shared<ControlConnection>(Private{}, std::move(executor), std::move(options));
    }

    ControlConnection::ControlConnection(Private, asio::any_io_executor executor, ControlConnectionOptions options)
        : options_{std::move(options)}
        , strand_{asio::make_strand(std::move(executor))}
        , transport_{std::make_shared<Transport>(strand_)}
        , framer_{options_.replyLimits}
        , deadlineTimer_{strand_}
        , settleTimer_{strand_}
        , readBuffer_{std::make_shared<std::array<char, 4096>>()}
    {}

    ControlConnection::~ControlConnection()
    {
        // The socket and pending commands are used from strand(), the destructor may run elsewhere.
        asio::post(
            strand_,
            [transport = std::move(transport_), awaiting = std::move(awaiting_), queued = std::move(queued_)]()
            {
                transport->close();
                const auto error = errorOf(RemoteErrorKind::Cancelled, "The control connection was destroyed");
                for (auto const& pending : awaiting)
                    pending->fail(error);
                for (auto const& pending : queued)
                    pending->fail(error);
            }
        );
    }

    asio::strand<asio::any_io_executor> const& ControlConnection::strand() const
    {
        return strand_;
    }

    asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> ControlConnection::connect(
        std::vector<asio::ip::tcp::endpoint> endpoints,
        std::optional<TlsConnectionSettings> implicitTls
    )
    {
        assert(strand_.running_in_this_thread() && "ControlConnection::connect() runs on the strand of the connection");
        if (state_ == ControlState::Closed)
            co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The control connection was closed"));
        if (state_ != ControlState::Unconnected)
            co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "The control connection was connected before"));
        if (endpoints.empty())
        {
            auto failure = errorOf(RemoteErrorKind::NoConnection, "There is no address to connect to");
            die(failure);
            co_return std::unexpected(std::move(failure));
        }
        std::optional<SslHandle> tlsHandle{};
        if (implicitTls)
        {
            auto handle = makeTlsHandle(*implicitTls);
            if (!handle)
            {
                auto failure = asNoSecureConnection(std::move(handle).error());
                die(failure);
                co_return std::unexpected(std::move(failure));
            }
            tlsHandle = std::move(*handle);
        }

        const auto deadline = Clock::now() + options_.connectTimeout;
        const auto endpoint = co_await connectToFirst(endpoints, deadline);
        if (!endpoint)
        {
            die(endpoint.error());
            co_return std::unexpected(endpoint.error());
        }
        remote_ = *endpoint;
        state_ = ControlState::Open;
        if (const auto configured = applyControlSocketOptions(transport_->socket(), options_.socket); !configured)
        {
            die(configured.error());
            co_return std::unexpected(configured.error());
        }
        if (tlsHandle)
        {
            if (auto secured = co_await handshake(std::move(*tlsHandle), deadline); !secured)
                co_return std::unexpected(asNoSecureConnection(std::move(secured).error()));
        }

        // The greeting is owed without a command, and within the connect timeout, a 120 before it included.
        const auto greeting = std::make_shared<PendingCommand>(strand_, std::nullopt, std::string{});
        greeting->stage_ = PendingCommand::Stage::Written;
        greeting->deadline_ = deadline;
        awaiting_.push_back(greeting);
        armDeadline();
        readNext();

        auto reply = co_await greeting->finalReply();
        if (!reply)
        {
            die(reply.error());
            co_return reply;
        }
        if (reply->code != 220)
        {
            auto failure = replyError(*reply, std::nullopt);
            die(failure);
            co_return std::unexpected(std::move(failure));
        }
        co_return reply;
    }

    asio::awaitable<std::expected<asio::ip::tcp::endpoint, RemoteIo::RemoteError>>
    ControlConnection::connectToFirst(std::vector<asio::ip::tcp::endpoint> const& endpoints, Clock::time_point deadline)
    {
        boost::system::error_code lastError{};
        for (std::size_t index = 0u; index < endpoints.size() && Clock::now() < deadline; ++index)
        {
            // Each endpoint gets its share of the time left, so one that never answers leaves time for the rest.
            const auto share = (deadline - Clock::now()) / static_cast<Clock::rep>(endpoints.size() - index);
            transport_->close();
            const auto [error] =
                co_await transport_->socket().async_connect(endpoints[index], asio::cancel_after(share, useTuple));
            // close() ran while the connect was under way.
            if (!isLive())
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The connect was cancelled"));
            // A cancelled coroutine would throw at its next co_await.
            if (isCancelled(co_await asio::this_coro::cancellation_state))
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The connect was cancelled"));
            if (!error)
                co_return endpoints[index];
            lastError = error;
        }
        co_return std::unexpected(connectError(lastError, deadline, options_.connectTimeout));
    }

    asio::awaitable<std::expected<TrustedCertificate, RemoteIo::RemoteError>>
    ControlConnection::startTls(TlsConnectionSettings settings)
    {
        assert(
            strand_.running_in_this_thread() && "ControlConnection::startTls() runs on the strand of the connection"
        );
        if (transport_->secured() || upgrading_)
            co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "The control connection has or starts TLS"));
        // From here on the connection ends up secured or ends.
        if (state_ != ControlState::Open)
            co_return std::unexpected(
                endTls(failure_.value_or(errorOf(RemoteErrorKind::NoConnection, "The control connection is not open")))
            );
        if (!awaiting_.empty() || !queued_.empty())
            co_return std::unexpected(
                endTls(errorOf(RemoteErrorKind::Failure, "AUTH TLS waits for an idle connection"))
            );
        auto handle = makeTlsHandle(settings);
        if (!handle)
            co_return std::unexpected(endTls(std::move(handle).error()));

        const auto pending = enqueue(Command{.verb = Verb::Authenticate, .argument = "TLS"});
        if (!pending)
            co_return std::unexpected(endTls(pending.error()));
        upgrading_ = true;
        const auto reply = co_await (*pending)->finalReply();
        // A cancel that came with the reply counts too, the next co_await would throw.
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return std::unexpected(endTls(errorOf(RemoteErrorKind::Cancelled, "The start of TLS was cancelled")));
        if (!reply)
            co_return std::unexpected(endTls(reply.error()));
        if (!acceptsAuthentication(reply->code))
            co_return std::unexpected(endTls(refusedAuthentication(*reply)));
        if (!isLive())
            co_return std::unexpected(
                endTls(failure_.value_or(errorOf(RemoteErrorKind::Cancelled, "The control connection was closed")))
            );
        if (framer_.inReply() || framer_.holdsPossibleEnd())
            co_return std::unexpected(endTls(protocolViolation(
                "The server sent bytes behind its reply to AUTH TLS, they could be injected", reply->code
            )));

        auto trusted = co_await handshake(std::move(*handle), Clock::now() + options_.commandTimeout);
        if (!trusted)
            co_return std::unexpected(endTls(std::move(trusted).error()));
        upgrading_ = false;
        readerPaused_ = false;
        readNext();
        co_return trusted;
    }

    asio::awaitable<std::expected<TrustedCertificate, RemoteIo::RemoteError>>
    ControlConnection::handshake(SslHandle handle, Clock::time_point deadline)
    {
        const auto transport = transport_;
        transport->secure(std::move(handle));
        const auto timeLeft = std::max(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds{0}
        );
        const auto [error] = co_await transport->tls().async_handshake(
            asio::ssl::stream_base::client, asio::cancel_after(timeLeft, useTuple)
        );
        // close() ran during the handshake.
        if (!isLive())
            co_return std::unexpected(
                failure_.value_or(errorOf(RemoteErrorKind::Cancelled, "The TLS handshake was cancelled"))
            );
        if (isCancelled(co_await asio::this_coro::cancellation_state))
        {
            auto cancelled = errorOf(RemoteErrorKind::Cancelled, "The TLS handshake was cancelled");
            die(cancelled);
            co_return std::unexpected(std::move(cancelled));
        }
        if (error)
        {
            untrustedCertificate_ = refusedCertificate(transport->tlsHandle());
            auto failure = handshakeError(transport->tlsHandle(), error, error == asio::error::operation_aborted);
            die(failure);
            co_return std::unexpected(std::move(failure));
        }
        auto trusted = trustOf(transport->tlsHandle());
        if (!trusted)
        {
            die(trusted.error());
            co_return std::unexpected(trusted.error());
        }
        trustedCertificate_ = *trusted;
        co_return trusted;
    }

    RemoteIo::RemoteError ControlConnection::endTls(RemoteIo::RemoteError error)
    {
        upgrading_ = false;
        if (state_ == ControlState::Closed)
            return errorOf(RemoteErrorKind::Cancelled, "The control connection was closed");
        auto failure = asNoSecureConnection(std::move(error));
        die(failure);
        return failure;
    }

    std::expected<std::shared_ptr<PendingCommand>, RemoteIo::RemoteError> ControlConnection::send(Command command)
    {
        assert(strand_.running_in_this_thread() && "ControlConnection::send() runs on the strand of the connection");
        if (command.verb == Verb::Authenticate)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "AUTH goes through startTls()"));
        if (upgrading_)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "The control connection is starting TLS"));
        return enqueue(std::move(command));
    }

    std::expected<std::shared_ptr<PendingCommand>, RemoteIo::RemoteError> ControlConnection::enqueue(Command command)
    {
        if (state_ != ControlState::Open)
            return std::unexpected(
                failure_.value_or(errorOf(RemoteErrorKind::NoConnection, "The control connection is not open"))
            );
        auto bytes = command.encode();
        if (!bytes)
            return std::unexpected(bytes.error());

        auto pending = std::make_shared<PendingCommand>(strand_, std::move(command), std::move(*bytes));
        queued_.push_back(pending);
        writeNext();
        return pending;
    }

    asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> ControlConnection::execute(Command command)
    {
        // The owner may drop the connection during the wait, the command then fails as Cancelled.
        const auto weak = weak_from_this();
        const auto pending = send(std::move(command));
        if (!pending)
            co_return std::unexpected(pending.error());
        auto reply = co_await (*pending)->finalReply();
        if (!reply && reply.error().kind == RemoteErrorKind::Cancelled)
        {
            if (const auto self = weak.lock())
                self->withdraw(**pending);
        }
        co_return reply;
    }

    bool ControlConnection::withdraw(PendingCommand const& pending)
    {
        const auto found = std::ranges::find_if(
            queued_,
            [&pending](std::shared_ptr<PendingCommand> const& queued)
            {
                return queued.get() == &pending;
            }
        );
        if (found == queued_.end())
            return false;
        (*found)->fail(errorOf(RemoteErrorKind::Cancelled, "The command was withdrawn before it was written"));
        queued_.erase(found);
        writeNext();
        return true;
    }

    void ControlConnection::limitFinalReply(PendingCommand& pending, std::chrono::milliseconds timeout)
    {
        const auto awaited = std::ranges::any_of(
            awaiting_,
            [&pending](std::shared_ptr<PendingCommand> const& awaiting)
            {
                return awaiting.get() == &pending;
            }
        );
        if (!awaited)
            return;
        pending.finalDeadline_ = Clock::now() + timeout;
        pending.deadline_ = pending.finalDeadline_;
        armDeadline();
    }

    void ControlConnection::close()
    {
        asio::dispatch(
            strand_,
            [weak = weak_from_this()]()
            {
                const auto self = weak.lock();
                if (!self)
                    return;
                self->end(
                    ControlState::Closed, errorOf(RemoteErrorKind::Cancelled, "The control connection was closed")
                );
            }
        );
    }

    void ControlConnection::onEnd(EndHandler handler)
    {
        assert(state_ == ControlState::Unconnected && "ControlConnection::onEnd() is set before connect()");
        endHandler_ = std::move(handler);
    }

    ControlState ControlConnection::state() const
    {
        return state_;
    }

    std::optional<RemoteIo::RemoteError> const& ControlConnection::failure() const
    {
        return failure_;
    }

    asio::ip::tcp::endpoint ControlConnection::remoteEndpoint() const
    {
        return remote_;
    }

    std::optional<TrustedCertificate> const& ControlConnection::trustedCertificate() const
    {
        return trustedCertificate_;
    }

    std::optional<CertificateInfo> const& ControlConnection::untrustedCertificate() const
    {
        return untrustedCertificate_;
    }

    std::optional<SslSessionHandle> ControlConnection::sessionForData()
    {
        assert(
            strand_.running_in_this_thread() &&
            "ControlConnection::sessionForData() runs on the strand of the connection"
        );
        if (!transport_->secured())
            return std::nullopt;
        return resumableSession(transport_->tlsHandle());
    }

    void ControlConnection::readNext()
    {
        transport_->readSome(
            asio::buffer(*readBuffer_),
            [weak = weak_from_this(),
                buffer = readBuffer_,
                transport = transport_](boost::system::error_code error, std::size_t received)
            {
                const auto self = weak.lock();
                if (!self)
                    return;
                self->takeRead(error, std::string_view{buffer->data(), received});
            }
        );
    }

    void ControlConnection::takeRead(boost::system::error_code error, std::string_view bytes)
    {
        if (!bytes.empty() && isLive())
            takeBytes(bytes);
        if (error)
            takeEndOfStream(error);
        else if (isLive() && !readerPaused_)
            readNext();
    }

    void ControlConnection::takeBytes(std::string_view bytes)
    {
        const auto fed = framer_.feed(bytes);
        deliverFramedReplies();
        if (!isLive())
            return;
        if (!fed)
        {
            die(fed.error());
            return;
        }
        if (framer_.holdsPossibleEnd())
            armSettle();
        else
            settleTimer_.cancel();
    }

    void ControlConnection::deliverFramedReplies()
    {
        auto reply = framer_.next();
        while (reply && isLive())
        {
            dispatch(std::move(*reply));
            reply = isLive() ? framer_.next() : std::nullopt;
        }
    }

    void ControlConnection::dispatch(Reply reply)
    {
        const auto code = reply.code;
        const auto preliminary = isPreliminary(code);
        // Only the greeting and transfers get 1yz replies (RFC 959 5.4). Some servers send a 150 after a transfer
        // failed, by then the next command may be written, and the end of the transfer is not in doubt.
        if (preliminary && (awaiting_.empty() || !takesPreliminaryReplies(*awaiting_.front())))
        {
            Log::debug("FTP: Dropped a {} reply that no command waited for", code);
            return;
        }
        if (awaiting_.empty())
        {
            Log::warn("FTP: A {} reply came while no command waited for one", code);
            if (code != 421)
            {
                die(protocolViolation(fmt::format("A {} reply came while no command waited for one", code), code));
                return;
            }
            // replyError() reads a 421 without a command as a refused greeting, an open session lost its connection.
            auto closing = replyError(reply, std::nullopt);
            if (closing.kind == RemoteErrorKind::NoConnection)
                closing.kind = RemoteErrorKind::ConnectionLost;
            die(std::move(closing));
            return;
        }
        if (drain_)
        {
            takeDrainedReply(std::move(reply));
            return;
        }

        const auto pending = awaiting_.front();
        if (pending->replies_.size() >= PendingCommand::maximumHeldReplies)
        {
            die(protocolViolation(fmt::format("{} was sent more replies than it can hold", nameOf(*pending)), code));
            return;
        }
        const auto verb = pending->verb();
        if (preliminary)
            pending->deadline_ = deadlineAfterPreliminary(*pending);
        else
            awaiting_.pop_front();

        // A 421 ends the connection like a close (RFC 1123 4.1.2.11), the command still gets it as its reply.
        auto closing = code == 421 ? std::optional{replyError(reply, verb)} : std::nullopt;
        const auto quitAnswered = verb == Verb::Quit && classifyReply(code) == ReplyClass::PositiveCompletion;
        // What follows belongs to the handshake, startTls() takes it from the socket.
        if (verb == Verb::Authenticate && upgrading_ && acceptsAuthentication(code))
            readerPaused_ = true;
        pending->take(std::move(reply));
        if (closing)
        {
            die(std::move(*closing));
            return;
        }
        if (quitAnswered)
        {
            end(ControlState::Closed, errorOf(RemoteErrorKind::Cancelled, "The control connection ended with QUIT"));
            return;
        }
        armDeadline();
        writeNext();
    }

    void ControlConnection::takeDrainedReply(Reply reply)
    {
        const auto code = reply.code;
        if (isPreliminary(code))
        {
            Log::debug("FTP: Dropped a {} reply after ABOR", code);
            return;
        }
        if (code == 421)
        {
            die(replyError(reply, drain_->transfer->verb()));
            return;
        }
        if (code == markerReplyCode)
        {
            finishDrain(std::move(reply));
            return;
        }
        if (drain_->replies.size() >= PendingCommand::maximumHeldReplies)
        {
            die(protocolViolation("ABOR was sent more replies than it can hold", code));
            return;
        }
        drain_->replies.push_back(std::move(reply));
    }

    void ControlConnection::finishDrain(Reply markerReply)
    {
        auto drain = std::move(*drain_);
        drain_.reset();
        std::erase_if(
            awaiting_,
            [&drain](std::shared_ptr<PendingCommand> const& pending)
            {
                return pending == drain.transfer || pending == drain.abort || pending == drain.marker;
            }
        );
        if (drain.replies.empty())
        {
            const auto missing = unreadableReply("The server answered neither the aborted transfer nor ABOR");
            drain.transfer->fail(missing);
            drain.abort->fail(missing);
        }
        else
        {
            if (drain.replies.size() > 2u)
                Log::debug("FTP: Dropped {} replies between the transfer and ABOR", drain.replies.size() - 2u);
            // A single reply answers both, the server did not say which it meant.
            drain.transfer->take(drain.replies.front());
            drain.abort->take(std::move(drain.replies.back()));
        }
        drain.marker->take(std::move(markerReply));
        armDeadline();
        writeNext();
    }

    std::shared_ptr<PendingCommand> ControlConnection::makeMarker() const
    {
        Command marker{.verb = Verb::PrintWorkingDirectory};
        auto bytes = marker.encode();
        assert(bytes && "PWD without an argument always encodes");
        return std::make_shared<PendingCommand>(strand_, std::move(marker), std::move(*bytes));
    }

    void ControlConnection::takeEndOfStream(boost::system::error_code error)
    {
        if (!isLive())
            return;
        if (error != asio::error::eof)
        {
            die(transportError("Reading from", error));
            return;
        }
        const auto finished = framer_.finish();
        deliverFramedReplies();
        if (!isLive())
            return;
        die(finished ? errorOf(RemoteErrorKind::ConnectionLost, "The server closed the control connection")
                     : finished.error());
    }

    void ControlConnection::writeNext()
    {
        if (writing_ || state_ != ControlState::Open)
            return;
        const auto next = nextToWrite();
        if (next == queued_.end())
            return;

        std::vector<std::shared_ptr<PendingCommand>> written{*next};
        queued_.erase(next);
        if (const auto transfer = runningTransfer(); transfer && written.front()->verb() == Verb::Abort)
        {
            drain_ = AbortDrain{.transfer = transfer, .abort = written.front(), .marker = makeMarker()};
            written.push_back(drain_->marker);
        }

        std::vector<asio::const_buffer> buffers{};
        for (auto const& pending : written)
        {
            pending->stage_ = PendingCommand::Stage::Written;
            pending->deadline_ = Clock::now() + options_.commandTimeout;
            awaiting_.push_back(pending);
            buffers.push_back(asio::buffer(pending->bytes_));
            Log::debug("FTP: Sending {}", pending->command_->toLogString());
        }
        armDeadline();
        writing_ = true;
        transport_->write(
            buffers,
            [weak = weak_from_this(),
                written = std::move(written),
                transport = transport_](boost::system::error_code error, std::size_t)
            {
                const auto self = weak.lock();
                if (!self)
                    return;
                self->writing_ = false;
                if (error)
                {
                    self->die(transportError("Writing to", error));
                    return;
                }
                self->writeNext();
            }
        );
    }

    std::deque<std::shared_ptr<PendingCommand>>::iterator ControlConnection::nextToWrite()
    {
        if (awaiting_.empty())
            return queued_.begin();
        if (!runningTransfer())
            return queued_.end();
        // Only ABOR goes into the transfer, ahead of commands queued before it (RFC 959 4.2). STAT and QUIT wait:
        // proftpd and pyftpdlib answer them at once, before the final reply of the transfer, so the replies would swap.
        return std::ranges::find_if(
            queued_,
            [](std::shared_ptr<PendingCommand> const& queued)
            {
                return queued->verb() == Verb::Abort;
            }
        );
    }

    std::shared_ptr<PendingCommand> ControlConnection::runningTransfer() const
    {
        const auto everyAnswered = std::ranges::all_of(
            awaiting_,
            [](std::shared_ptr<PendingCommand> const& pending)
            {
                return pending->answered();
            }
        );
        if (!everyAnswered)
            return nullptr;
        const auto transfer = std::ranges::find_if(awaiting_, isTransferCommand);
        if (transfer == awaiting_.end())
            return nullptr;
        return *transfer;
    }

    void ControlConnection::armDeadline()
    {
        std::optional<Clock::time_point> earliest{};
        for (auto const& pending : awaiting_)
        {
            if (pending->deadline_ && (!earliest || *pending->deadline_ < *earliest))
                earliest = pending->deadline_;
        }
        if (earliest == armedDeadline_)
            return;
        armedDeadline_ = earliest;
        if (!earliest)
        {
            deadlineTimer_.cancel();
            return;
        }
        deadlineTimer_.expires_at(*earliest);
        deadlineTimer_.async_wait(
            [weak = weak_from_this()](boost::system::error_code error)
            {
                if (error)
                    return;
                if (const auto self = weak.lock())
                    self->takeDeadline();
            }
        );
    }

    void ControlConnection::takeDeadline()
    {
        armedDeadline_.reset();
        if (!isLive())
            return;
        const auto now = Clock::now();
        const auto late = std::ranges::find_if(
            awaiting_,
            [now](std::shared_ptr<PendingCommand> const& pending)
            {
                return pending->deadline_ && *pending->deadline_ <= now;
            }
        );
        if (late == awaiting_.end())
        {
            armDeadline();
            return;
        }
        die(errorOf(RemoteErrorKind::TimedOut, fmt::format("No reply to {} came in time", nameOf(**late))));
    }

    void ControlConnection::armSettle()
    {
        settleTimer_.expires_after(options_.settleTime);
        settleTimer_.async_wait(
            [weak = weak_from_this()](boost::system::error_code error)
            {
                const auto self = error ? nullptr : weak.lock();
                if (!self || !self->isLive())
                    return;
                self->framer_.settle();
                self->deliverFramedReplies();
            }
        );
    }

    std::optional<Clock::time_point> ControlConnection::deadlineAfterPreliminary(PendingCommand const& pending)
    {
        // Only the greeting and transfers get here. The greeting keeps the connect deadline, a transfer has none until
        // limitFinalReply(), whose limit a late 1yz keeps.
        if (!pending.verb())
            return pending.deadline_;
        return pending.finalDeadline_;
    }

    void ControlConnection::die(RemoteIo::RemoteError error)
    {
        end(ControlState::Dead, std::move(error));
    }

    void ControlConnection::end(ControlState state, RemoteIo::RemoteError error)
    {
        if (!isLive())
            return;
        state_ = state;
        if (state == ControlState::Dead)
        {
            Log::debug("FTP: Control connection ended: {}", RemoteIo::toString(error.kind));
            failure_ = error;
        }
        // A failed handshake leaves no TLS to close.
        if (state == ControlState::Closed && trustedCertificate_)
            Transport::closeAfterNotify(transport_, options_.closeNotifyTimeout);
        else
            transport_->close();
        deadlineTimer_.cancel();
        armedDeadline_.reset();
        settleTimer_.cancel();
        drain_.reset();
        const auto awaiting = std::exchange(awaiting_, {});
        const auto queued = std::exchange(queued_, {});
        for (auto const& pending : awaiting)
            pending->fail(error);
        for (auto const& pending : queued)
            pending->fail(error);
        if (endHandler_)
        {
            asio::post(
                strand_,
                [handler = std::exchange(endHandler_, {}), error = std::move(error)]()
                {
                    handler(error);
                }
            );
        }
    }

    bool ControlConnection::isLive() const
    {
        return state_ == ControlState::Unconnected || state_ == ControlState::Open;
    }
}
