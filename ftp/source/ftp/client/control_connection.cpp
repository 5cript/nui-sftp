#include <ftp/client/control_connection.hpp>
#include <ftp/protocol/protocol_error.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <log/log.hpp>
#include "client_support.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
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
#include <cstddef>
#include <optional>
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
        using Detail::errorOf;
        using Detail::isCancelled;
        using Detail::useTuple;

        /** @brief The reply of PWD, the marker behind ABOR. No reply to a transfer or to ABOR has this code. */
        constexpr int markerReplyCode = 257;

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

        RemoteIo::RemoteError injectedBytes()
        {
            return errorOf(
                RemoteErrorKind::NoSecureConnection,
                "The server sent bytes behind its reply to AUTH TLS, they could be injected"
            );
        }

        RemoteIo::RemoteError refusedAuthentication(Reply const& reply)
        {
            auto error = replyError(reply, Verb::Authenticate);
            error.message = fmt::format("The server refused AUTH TLS: {}", error.message);
            return error;
        }

        /** @brief Enough for the line a server refuses a connection with. */
        constexpr std::size_t recordedBytes = 512u;

        /**
         * @brief The socket under the TLS stream, keeping the first bytes the server sent. A server that refuses an
         *        implicit TLS connection answers in plaintext, vsftpd over its limit with a 421.
         */
        class RecordingSocket
        {
          public:
            using executor_type = asio::ip::tcp::socket::executor_type;
            using lowest_layer_type = asio::ip::tcp::socket::lowest_layer_type;

            RecordingSocket(asio::ip::tcp::socket& socket, std::string& received)
                : socket_{&socket}
                , received_{&received}
            {}

            executor_type get_executor()
            {
                return socket_->get_executor();
            }

            lowest_layer_type& lowest_layer()
            {
                return socket_->lowest_layer();
            }

            lowest_layer_type const& lowest_layer() const
            {
                return socket_->lowest_layer();
            }

            template <typename BuffersT, typename HandlerT>
            void async_read_some(BuffersT const& buffers, HandlerT&& handler)
            {
                if (received_->size() >= recordedBytes)
                {
                    socket_->async_read_some(buffers, std::forward<HandlerT>(handler));
                    return;
                }
                auto slot = asio::get_associated_cancellation_slot(handler);
                auto executor = asio::get_associated_executor(handler, socket_->get_executor());
                socket_->async_read_some(
                    buffers,
                    asio::bind_cancellation_slot(
                        slot,
                        asio::bind_executor(
                            executor,
                            [received = received_, buffers, handler = std::forward<HandlerT>(handler)](
                                boost::system::error_code error, std::size_t size
                            ) mutable
                            {
                                std::string bytes(std::min(size, recordedBytes - received->size()), '\0');
                                asio::buffer_copy(asio::buffer(bytes), buffers);
                                received->append(bytes);
                                std::move(handler)(error, size);
                            }
                        )
                    )
                );
            }

            template <typename BuffersT, typename HandlerT>
            void async_write_some(BuffersT const& buffers, HandlerT&& handler)
            {
                socket_->async_write_some(buffers, std::forward<HandlerT>(handler));
            }

          private:
            asio::ip::tcp::socket* socket_;
            std::string* received_;
        };
    }

    /**
     * @brief The socket and, once there is one, the TLS stream over it. Every operation in flight holds it next to the
     *        connection, so it outlives them. Used on the strand of the connection only.
     */
    class ControlConnection::Transport : public std::enable_shared_from_this<Transport>
    {
      public:
        explicit Transport(asio::strand<asio::any_io_executor> const& strand)
            : socket_{strand}
            , closeTimer_{strand}
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
            tls_.emplace(RecordingSocket{socket_, received_}, handle.release());
        }

        asio::ssl::stream<RecordingSocket>& tls()
        {
            return *tls_;
        }

        /**
         * @brief The negative reply the server sent instead of taking part in TLS. Nothing for bytes that are no
         *        reply and for any other reply: a greeting in plaintext is a server without TLS on this port.
         */
        std::optional<Reply> plaintextRefusal() const
        {
            ReplyFramer framer{};
            if (!framer.feed(received_))
                return std::nullopt;
            auto reply = framer.next();
            if (!reply)
                return std::nullopt;
            const auto replyClass = classifyReply(reply->code);
            if (replyClass != ReplyClass::TransientNegative && replyClass != ReplyClass::PermanentNegative)
                return std::nullopt;
            return reply;
        }

        SSL* tlsHandle()
        {
            return tls_ ? tls_->native_handle() : nullptr;
        }

        template <typename HandlerT>
        void readSome(asio::mutable_buffer buffer, HandlerT&& handler)
        {
            reading_ = true;
            auto read = [transport = shared_from_this(), handler = std::forward<HandlerT>(handler)](
                            boost::system::error_code error, std::size_t size
                        ) mutable
            {
                transport->reading_ = false;
                if (transport->sendingShutDown_)
                    transport->drain();
                handler(error, size);
            };
            if (tls_)
                tls_->async_read_some(buffer, std::move(read));
            else
                socket_.async_read_some(buffer, std::move(read));
        }

        template <typename BuffersT, typename HandlerT>
        void write(BuffersT const& buffers, HandlerT&& handler)
        {
            writing_ = true;
            auto written = [transport = shared_from_this(), handler = std::forward<HandlerT>(handler)](
                               boost::system::error_code error, std::size_t size
                           ) mutable
            {
                transport->writing_ = false;
                if (transport->notifyAfterWrite_)
                    transport->sendCloseNotify();
                handler(error, size);
            };
            if (tls_)
                asio::async_write(*tls_, buffers, std::move(written));
            else
                asio::async_write(socket_, buffers, std::move(written));
        }

        void close()
        {
            boost::system::error_code ignored{};
            socket_.close(ignored);
            closeTimer_.cancel();
        }

        /**
         * @brief Sends close_notify without waiting for the server's, shuts the sending side down and closes once the
         *        server ended its side, all within @p timeout. A write in flight goes first: Asio's ssl::stream drops
         *        the output of a shutdown that had to wait for one. Closing the socket before the server took the sent
         *        bytes can reset the connection on Windows, which loses them.
         */
        void closeAfterNotify(std::chrono::milliseconds timeout)
        {
            // The server's close_notify counts as received, the shutdown ends once ours is out.
            SSL_set_shutdown(tls_->native_handle(), SSL_RECEIVED_SHUTDOWN);
            closeTimer_.expires_after(timeout);
            closeTimer_.async_wait(
                [transport = shared_from_this()](boost::system::error_code error)
                {
                    if (!error)
                        transport->close();
                }
            );
            if (writing_)
                notifyAfterWrite_ = true;
            else
                sendCloseNotify();
        }

      private:
        void sendCloseNotify()
        {
            notifyAfterWrite_ = false;
            if (!socket_.is_open())
                return;
            tls_->async_shutdown(
                [transport = shared_from_this()](boost::system::error_code)
                {
                    transport->shutDownSending();
                }
            );
        }

        void shutDownSending()
        {
            boost::system::error_code ignored{};
            socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
            sendingShutDown_ = true;
            // A read under way sees the end first and drains on.
            if (!reading_)
                drain();
        }

        /**
         * @brief Reads past the end of TLS until the server closes its side, then closes.
         */
        void drain()
        {
            if (!socket_.is_open())
                return;
            socket_.async_read_some(
                asio::buffer(drainBuffer_),
                [transport = shared_from_this()](boost::system::error_code error, std::size_t)
                {
                    if (error)
                        transport->close();
                    else
                        transport->drain();
                }
            );
        }

      private:
        asio::ip::tcp::socket socket_;
        /** @brief The first bytes the TLS stream read, see RecordingSocket. */
        std::string received_{};
        std::optional<asio::ssl::stream<RecordingSocket>> tls_{};
        /** @brief Bounds a close over TLS. */
        asio::steady_timer closeTimer_;
        /** @brief Takes what the server sends after the end of TLS, the drain holds the transport. */
        std::array<char, 512> drainBuffer_{};
        bool reading_{false};
        bool writing_{false};
        /** @brief close_notify went out and the sending side is shut, the rest is waiting for the server's end. */
        bool sendingShutDown_{false};
        /** @brief close_notify waits for the write in flight. */
        bool notifyAfterWrite_{false};
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
            // Nothing goes out before the greeting, and nothing into the handshake.
            upgrading_ = true;
            auto secured = co_await handshake(std::move(*tlsHandle), deadline, true);
            upgrading_ = false;
            if (!secured && transport_->plaintextRefusal())
                co_return std::unexpected(std::move(secured).error());
            if (!secured)
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
        // A cancel that came with the greeting counts too, the next co_await would throw.
        if (isCancelled(co_await asio::this_coro::cancellation_state))
        {
            auto cancelled = errorOf(RemoteErrorKind::Cancelled, "The connect was cancelled");
            die(cancelled);
            co_return std::unexpected(std::move(cancelled));
        }
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
            co_return std::unexpected(endTls(injectedBytes()));

        auto trusted = co_await handshake(std::move(*handle), Clock::now() + options_.commandTimeout, false);
        if (!trusted)
            co_return std::unexpected(endTls(std::move(trusted).error()));
        upgrading_ = false;
        readerPaused_ = false;
        readNext();
        co_return trusted;
    }

    asio::awaitable<std::expected<TrustedCertificate, RemoteIo::RemoteError>>
    ControlConnection::handshake(SslHandle handle, Clock::time_point deadline, bool plaintextRefusal)
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
            if (const auto refusal = plaintextRefusal ? transport->plaintextRefusal() : std::nullopt)
            {
                Log::debug("FTP: The server answered TLS with a {} reply in plaintext", refusal->code);
                failure = replyError(*refusal, std::nullopt);
            }
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
        reading_ = true;
        transport_->readSome(
            asio::buffer(*readBuffer_),
            [weak = weak_from_this(),
                buffer = readBuffer_,
                transport = transport_](boost::system::error_code error, std::size_t received)
            {
                const auto self = weak.lock();
                if (!self)
                    return;
                self->reading_ = false;
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
        // While TLS starts the bytes go in one at a time, any behind the reply to AUTH could have been injected.
        const auto piece = upgrading_ ? std::size_t{1u} : bytes.size();
        for (std::size_t offset = 0u; offset < bytes.size(); offset += piece)
        {
            if (readerPaused_)
            {
                die(injectedBytes());
                return;
            }
            const auto fed = framer_.feed(bytes.substr(offset, piece));
            deliverFramedReplies();
            if (!isLive())
                return;
            if (!fed)
            {
                die(fed.error());
                return;
            }
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
        // Nothing may follow the reply to AUTH in plaintext.
        if (readerPaused_)
        {
            die(injectedBytes());
            return;
        }
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
        const auto startsTls = verb == Verb::Authenticate && upgrading_ && acceptsAuthentication(code);
        // Ended by the settle time, the read under way would take the first bytes of the handshake.
        if (startsTls && reading_)
        {
            die(errorOf(
                RemoteErrorKind::NoSecureConnection,
                "The reply to AUTH TLS ended only after a quiet time, a read was under way"
            ));
            return;
        }
        if (preliminary)
            pending->deadline_ = deadlineAfterPreliminary(*pending);
        else
            awaiting_.pop_front();

        // A 421 ends the connection like a close (RFC 1123 4.1.2.11), the command still gets it as its reply.
        auto closing = code == 421 ? std::optional{replyError(reply, verb)} : std::nullopt;
        const auto quitAnswered = verb == Verb::Quit && classifyReply(code) == ReplyClass::PositiveCompletion;
        // What follows belongs to the handshake, startTls() takes it from the socket.
        if (startsTls)
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
        auto timeout = options_.commandTimeout;
        if (const auto transfer = runningTransfer(); transfer && written.front()->verb() == Verb::Abort)
        {
            drain_ = AbortDrain{.transfer = transfer, .abort = written.front(), .marker = makeMarker()};
            written.push_back(drain_->marker);
            timeout = options_.drainTimeout;
        }

        std::vector<asio::const_buffer> buffers{};
        for (auto const& pending : written)
        {
            pending->stage_ = PendingCommand::Stage::Written;
            pending->deadline_ = Clock::now() + timeout;
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
            transport_->closeAfterNotify(options_.closeNotifyTimeout);
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
