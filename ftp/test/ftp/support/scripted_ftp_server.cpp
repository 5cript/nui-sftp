#include <ftp/support/scripted_ftp_server.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <fmt/format.h>

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <regex>
#include <thread>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        namespace asio = boost::asio;
        namespace ssl = boost::asio::ssl;
        using boost::system::error_code;
        using tcp = asio::ip::tcp;
        using Kind = TranscriptEntry::Kind;

        constexpr auto noThrow = asio::as_tuple(asio::use_awaitable);
        constexpr std::chrono::seconds closeNotifyWait{2};
        constexpr std::size_t foreverBlockSize = 64u * 1024u;

        /**
         * @brief A flag a coroutine can wait for. Everything runs on the server thread, so it needs no lock.
         */
        class Event
        {
          public:
            explicit Event(asio::any_io_executor executor)
                : timer_{std::move(executor), asio::steady_timer::time_point::max()}
            {}

            void set()
            {
                isSet_ = true;
                timer_.cancel();
            }

            asio::awaitable<void> wait()
            {
                while (!isSet_)
                    co_await timer_.async_wait(noThrow);
            }

          private:
            bool isSet_{false};
            asio::steady_timer timer_;
        };

        /**
         * @brief A connection that may turn to TLS on the way, the stream does not move when it does.
         */
        class Transport
        {
          public:
            explicit Transport(tcp::socket socket)
                : socket_{std::move(socket)}
            {
                error_code ignored;
                socket_.set_option(tcp::no_delay{true}, ignored);
            }

            tcp::socket& socket()
            {
                return socket_;
            }

            asio::awaitable<std::pair<error_code, std::size_t>> readSome(asio::mutable_buffer buffer)
            {
                if (tls_)
                {
                    const auto [error, size] = co_await tls_->async_read_some(buffer, noThrow);
                    co_return std::pair{error, size};
                }
                const auto [error, size] = co_await socket_.async_read_some(buffer, noThrow);
                co_return std::pair{error, size};
            }

            asio::awaitable<error_code> writeAll(asio::const_buffer buffer)
            {
                if (tls_)
                {
                    const auto [error, size] = co_await asio::async_write(*tls_, buffer, noThrow);
                    co_return error;
                }
                const auto [error, size] = co_await asio::async_write(socket_, buffer, noThrow);
                co_return error;
            }

            asio::awaitable<error_code> handshake(std::shared_ptr<ssl::context> context)
            {
                context_ = std::move(context);
                tls_.emplace(socket_, *context_);
                const auto [error] = co_await tls_->async_handshake(ssl::stream_base::server, noThrow);
                co_return error;
            }

            bool resumed()
            {
                return tls_ && SSL_session_reused(tls_->native_handle()) == 1;
            }

            /**
             * @brief Sends a HelloRequest, the client's handshake is taken in by the next read.
             */
            asio::awaitable<bool> requestRenegotiation()
            {
                if (!tls_ || SSL_renegotiate(tls_->native_handle()) != 1)
                    co_return false;
                const auto [error] = co_await tls_->async_handshake(ssl::stream_base::server, noThrow);
                co_return !error;
            }

            bool renegotiationPending()
            {
                return tls_ && SSL_renegotiate_pending(tls_->native_handle()) == 1;
            }

            std::string tlsVersion()
            {
                return tls_ ? SSL_get_version(tls_->native_handle()) : "";
            }

            /**
             * @brief Closes the connection, over TLS with close_notify unless @p notify is false.
             */
            asio::awaitable<void> close(bool notify)
            {
                error_code ignored;
                // async_shutdown waits for the close_notify of the peer without a limit.
                if (tls_ && notify)
                    co_await tls_->async_shutdown(asio::cancel_after(closeNotifyWait, noThrow));
                socket_.shutdown(tcp::socket::shutdown_send, ignored);
                socket_.close(ignored);
            }

            void reset()
            {
                error_code ignored;
                socket_.set_option(asio::socket_base::linger{true, 0}, ignored);
                socket_.close(ignored);
            }

          private:
            tcp::socket socket_;
            std::shared_ptr<ssl::context> context_{};
            std::optional<ssl::stream<tcp::socket&>> tls_{};
        };

        std::string endReason(error_code error)
        {
            if (error == asio::error::eof)
                return "end of stream";
            if (error == ssl::error::stream_truncated)
                return "truncated";
            if (error == asio::error::connection_reset)
                return "reset";
            return error.message();
        }

        /**
         * @brief Opens @p acceptor on @p endpoint. No SO_REUSEADDR, with it two processes can get the same free port.
         */
        error_code openAcceptor(tcp::acceptor& acceptor, tcp::endpoint const& endpoint)
        {
            error_code error;
            acceptor.open(endpoint.protocol(), error);
            if (!error)
                acceptor.bind(endpoint, error);
            if (!error)
                acceptor.listen(asio::socket_base::max_listen_connections, error);
            return error;
        }

        std::shared_ptr<ssl::context> makeContext(CertifiedKey const& certified)
        {
            auto context = std::make_shared<ssl::context>(ssl::context::tls_server);
            context->use_certificate_chain(asio::buffer(certified.certificate));
            context->use_private_key(asio::buffer(certified.privateKey), ssl::context::pem);
            return context;
        }

        class ControlSession;
    }

    class ScriptedFtpServer::Implementation
    {
      public:
        Implementation(Options options, CertifiedKey certificate)
            : options_{std::move(options)}
            , certificate_{std::move(certificate)}
            , acceptor_{ioContext_}
        {}

        ~Implementation()
        {
            ioContext_.stop();
            if (thread_.joinable())
                thread_.join();
            // Timers of scripts still waiting for a release, they must go before ioContext_.
            releaseWaiters_.clear();
        }
        Implementation(Implementation const&) = delete;
        Implementation& operator=(Implementation const&) = delete;
        Implementation(Implementation&&) = delete;
        Implementation& operator=(Implementation&&) = delete;

        std::expected<void, std::string> open();

        std::string address() const
        {
            return options_.ipv6 ? "::1" : "127.0.0.1";
        }
        std::uint16_t port() const
        {
            return port_;
        }
        bool implicitTls() const
        {
            return options_.implicitTls;
        }
        asio::any_io_executor executor()
        {
            return ioContext_.get_executor();
        }

        void addConnection(Script script)
        {
            std::lock_guard lock{mutex_};
            scripts_.push_back(std::move(script));
            ++added_;
        }

        void record(std::size_t connection, Kind kind, std::string detail)
        {
            {
                std::lock_guard lock{mutex_};
                transcript_.push_back({.connection = connection, .kind = kind, .detail = std::move(detail)});
            }
            changed_.notify_all();
        }

        void fail(std::string description)
        {
            std::lock_guard lock{mutex_};
            failures_.push_back(std::move(description));
        }

        void reachCheckpoint(std::string name)
        {
            {
                std::lock_guard lock{mutex_};
                checkpoints_.push_back(std::move(name));
            }
            changed_.notify_all();
        }

        void scriptEnded()
        {
            {
                std::lock_guard lock{mutex_};
                ++ended_;
            }
            changed_.notify_all();
        }

        void connectionEnded()
        {
            {
                std::lock_guard lock{mutex_};
                ++disconnected_;
            }
            changed_.notify_all();
        }

        void release(std::string name)
        {
            std::vector<std::shared_ptr<asio::steady_timer>> waiting{};
            {
                std::lock_guard lock{mutex_};
                releases_.push_back(std::move(name));
                waiting = releaseWaiters_;
            }
            for (auto const& waiter : waiting)
            {
                asio::post(
                    executor(),
                    [waiter]()
                    {
                        waiter->cancel();
                    }
                );
            }
        }

        /**
         * @brief Takes a release of @p name, false when there is none yet.
         */
        bool takeRelease(std::string const& name)
        {
            std::lock_guard lock{mutex_};
            const auto found = std::ranges::find(releases_, name);
            if (found == releases_.end())
                return false;
            releases_.erase(found);
            return true;
        }

        /**
         * @brief Cancels @p waiter on the executor of the server whenever release() is called.
         */
        void wakeOnRelease(std::shared_ptr<asio::steady_timer> waiter)
        {
            std::lock_guard lock{mutex_};
            releaseWaiters_.push_back(std::move(waiter));
        }

        void stopWakingOnRelease(std::shared_ptr<asio::steady_timer> const& waiter)
        {
            std::lock_guard lock{mutex_};
            std::erase(releaseWaiters_, waiter);
        }

        bool waitForCheckpoint(std::string const& name, std::chrono::milliseconds timeout)
        {
            std::unique_lock lock{mutex_};
            const auto reached = [&]()
            {
                return std::ranges::find(checkpoints_, name) != checkpoints_.end();
            };
            if (!changed_.wait_for(lock, timeout, reached))
                return false;
            checkpoints_.erase(std::ranges::find(checkpoints_, name));
            return true;
        }

        bool waitForTranscriptEntry(Kind kind, std::chrono::milliseconds timeout)
        {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock,
                timeout,
                [&]()
                {
                    return std::ranges::any_of(
                        transcript_,
                        [kind](TranscriptEntry const& entry)
                        {
                            return entry.kind == kind;
                        }
                    );
                }
            );
        }

        bool waitUntilFinished(std::chrono::milliseconds timeout)
        {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock,
                timeout,
                [this]()
                {
                    return ended_ == added_;
                }
            );
        }

        bool waitUntilDisconnected(std::chrono::milliseconds timeout)
        {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock,
                timeout,
                [this]()
                {
                    return disconnected_ == nextConnection_;
                }
            );
        }

        std::vector<TranscriptEntry> transcript() const
        {
            std::lock_guard lock{mutex_};
            return transcript_;
        }

        std::vector<std::string> commands(std::size_t connection) const
        {
            std::lock_guard lock{mutex_};
            std::vector<std::string> lines{};
            for (auto const& entry : transcript_)
            {
                if (entry.connection == connection && entry.kind == Kind::Command)
                    lines.push_back(entry.detail);
            }
            return lines;
        }

        std::vector<std::string> failures() const
        {
            std::lock_guard lock{mutex_};
            return failures_;
        }

        /**
         * @brief The context for @p behaviour. Data connections resume sessions of the control connection only when
         *        they share its context, so this is the common one unless the behaviour needs another.
         */
        std::shared_ptr<ssl::context> contextFor(TlsBehaviour const& behaviour)
        {
            if (!behaviour.refuseResumption && !behaviour.issueNoTickets && !behaviour.certificate &&
                !behaviour.maximumVersion)
                return context_;
            auto context = makeContext(behaviour.certificate.value_or(certificate_));
            if (behaviour.maximumVersion)
            {
                SSL_CTX_set_max_proto_version(context->native_handle(), *behaviour.maximumVersion);
                // Versions before TLS 1.2 need the lowest security level of OpenSSL 3.
                if (*behaviour.maximumVersion < TLS1_2_VERSION)
                {
                    SSL_CTX_set_security_level(context->native_handle(), 0);
                    SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_VERSION);
                }
            }
            if (behaviour.refuseResumption || behaviour.issueNoTickets)
            {
                SSL_CTX_set_num_tickets(context->native_handle(), 0u);
                SSL_CTX_set_options(context->native_handle(), SSL_OP_NO_TICKET);
                SSL_CTX_set_session_cache_mode(context->native_handle(), SSL_SESS_CACHE_OFF);
            }
            return context;
        }

      private:
        asio::awaitable<void> acceptConnections();
        /**
         * @brief Runs the next script on a control connection that was just accepted.
         */
        void startSession(tcp::socket socket);

      private:
        const Options options_;
        const CertifiedKey certificate_;
        std::shared_ptr<ssl::context> context_{};
        std::uint16_t port_{0u};

        mutable std::mutex mutex_{};
        std::condition_variable changed_{};
        std::deque<Script> scripts_{};
        std::size_t added_{0u};
        std::size_t ended_{0u};
        std::size_t nextConnection_{0u};
        std::size_t disconnected_{0u};
        std::vector<TranscriptEntry> transcript_{};
        std::vector<std::string> checkpoints_{};
        std::vector<std::string> releases_{};
        std::vector<std::shared_ptr<asio::steady_timer>> releaseWaiters_{};
        std::vector<std::string> failures_{};

        // Destroyed first of what suspended coroutines may use, the coroutines go with it.
        asio::io_context ioContext_{1};
        tcp::acceptor acceptor_;
        std::thread thread_{};
    };

    namespace
    {
        using Server = ScriptedFtpServer::Implementation;

        enum class TlsOutcome
        {
            Established,
            HandshakeReplaced,
            Failed,
        };

        /**
         * @brief Does what @p behaviour says on @p transport and records how it went.
         *
         * @param label The connection in the transcript, "control" or "data".
         */
        asio::awaitable<TlsOutcome> secure(
            Server& server,
            std::size_t connection,
            Transport& transport,
            TlsBehaviour const& behaviour,
            std::string_view label
        )
        {
            if (!behaviour.plaintextFirst.empty())
                co_await transport.writeAll(asio::buffer(behaviour.plaintextFirst));
            if (behaviour.insteadOfHandshake)
            {
                co_await transport.writeAll(asio::buffer(*behaviour.insteadOfHandshake));
                co_return TlsOutcome::HandshakeReplaced;
            }
            if (const auto error = co_await transport.handshake(server.contextFor(behaviour)); error)
            {
                server.record(connection, Kind::TlsFailed, fmt::format("{}: {}", label, endReason(error)));
                co_return TlsOutcome::Failed;
            }
            server.record(
                connection,
                Kind::TlsEstablished,
                fmt::format("{} {}{}", label, transport.tlsVersion(), transport.resumed() ? " resumed" : "")
            );
            co_return TlsOutcome::Established;
        }

        /**
         * @brief Runs a DataPlan: waits for the client, then for the control script to begin it.
         */
        class DataSession : public std::enable_shared_from_this<DataSession>
        {
          public:
            DataSession(Server& server, std::size_t connection, DataPlan plan, asio::ip::address controlPeer)
                : server_{server}
                , connection_{connection}
                , plan_{std::move(plan)}
                , actions_{plan_.actions}
                , controlPeer_{std::move(controlPeer)}
                , acceptor_{server.executor()}
                , begun_{server.executor()}
                , finished_{server.executor()}
                , stoppedSignal_{server.executor()}
            {}

            /**
             * @brief Binds a free port on @p address.
             *
             * @return The port, or why there is none.
             */
            std::expected<std::uint16_t, std::string> listen(asio::ip::address const& address)
            {
                const tcp::endpoint endpoint{address, 0u};
                if (const auto opened = openAcceptor(acceptor_, endpoint); opened)
                    return std::unexpected(opened.message());
                return acceptor_.local_endpoint().port();
            }

            asio::awaitable<void> run()
            {
                const auto self = shared_from_this();
                co_await acceptClient();
                if (transport_)
                {
                    co_await begun_.wait();
                    if (!stopped_ && co_await startTls())
                    {
                        bool goesOn = !stopped_;
                        for (std::size_t action = 0u; goesOn && action < actions_.size(); ++action)
                        {
                            goesOn = co_await std::visit(
                                [this](auto const& step)
                                {
                                    return perform(step);
                                },
                                actions_[action]
                            );
                        }
                    }
                    if (!closed_)
                        co_await closeTransport();
                }
                finished_.set();
            }

            /**
             * @brief Starts the plan, with @p actions instead of its own where given.
             */
            void begin(std::optional<std::vector<DataPlan::Action>> actions)
            {
                if (actions)
                    actions_ = std::move(*actions);
                begun_.set();
            }

            /**
             * @brief Ends the plan. A shutdown because a cancel misses a composed write between two of its steps.
             */
            void stop()
            {
                stopped_ = true;
                stoppedSignal_.set();
                error_code ignored;
                acceptor_.close(ignored);
                if (transport_)
                    transport_->socket().shutdown(tcp::socket::shutdown_both, ignored);
                begun_.set();
            }

            asio::awaitable<void> awaitEnd()
            {
                co_await finished_.wait();
            }

          private:
            asio::awaitable<void> acceptClient()
            {
                error_code error{};
                while (!transport_ && !error)
                {
                    auto [acceptError, socket] = co_await acceptor_.async_accept(noThrow);
                    error = acceptError;
                    if (!error)
                    {
                        error_code peerError;
                        const auto peer = socket.remote_endpoint(peerError).address();
                        if (plan_.onlyControlPeer && peer != controlPeer_)
                            server_.record(connection_, Kind::DataRefused, peer.to_string());
                        else
                        {
                            server_.record(connection_, Kind::DataConnected, peer.to_string());
                            transport_.emplace(std::move(socket));
                        }
                    }
                }
                if (transport_)
                {
                    error_code ignored;
                    acceptor_.close(ignored);
                }
            }

            asio::awaitable<bool> startTls()
            {
                if (!plan_.tlsBehaviour)
                    co_return true;
                auto const& behaviour = *plan_.tlsBehaviour;
                const auto outcome = co_await secure(server_, connection_, *transport_, behaviour, "data");
                if (outcome != TlsOutcome::Established)
                    co_return false;
                if (behaviour.requireResumption && !transport_->resumed())
                {
                    server_.record(connection_, Kind::DataNotResumed, {});
                    co_return false;
                }
                co_return true;
            }

            asio::awaitable<void> closeTransport()
            {
                closed_ = true;
                const bool notify = !plan_.tlsBehaviour || !plan_.tlsBehaviour->closeWithoutCloseNotify;
                co_await transport_->close(notify);
            }

            asio::awaitable<bool> perform(DataPlan::Send const& step)
            {
                std::string_view rest{step.bytes};
                asio::steady_timer pause{server_.executor()};
                while (!rest.empty())
                {
                    const auto size = step.chunkSize == 0u ? rest.size() : std::min(step.chunkSize, rest.size());
                    if (const auto error = co_await transport_->writeAll(asio::buffer(rest.data(), size)); error)
                    {
                        server_.record(connection_, Kind::DataEnded, endReason(error));
                        co_return false;
                    }
                    rest.remove_prefix(size);
                    if (step.interval.count() > 0 && !rest.empty())
                    {
                        pause.expires_after(step.interval);
                        co_await pause.async_wait(noThrow);
                    }
                }
                co_return true;
            }

            asio::awaitable<bool> perform(DataPlan::SendForever const&)
            {
                const std::string block(foreverBlockSize, 'x');
                error_code error{};
                while (!error)
                    error = co_await transport_->writeAll(asio::buffer(block));
                if (!stopped_)
                    server_.record(connection_, Kind::DataEnded, endReason(error));
                co_return false;
            }

            asio::awaitable<bool> perform(DataPlan::ExpectUpload const& step)
            {
                std::string uploaded{};
                std::array<char, 16u * 1024u> chunk{};
                error_code error{};
                while (!error)
                {
                    const auto [readError, size] = co_await transport_->readSome(asio::buffer(chunk));
                    uploaded.append(chunk.data(), size);
                    error = readError;
                }
                if (uploaded.size() > step.limit)
                    server_.fail(
                        fmt::format("Upload of {} bytes exceeds the limit of {}", uploaded.size(), step.limit)
                    );
                server_.record(connection_, Kind::DataUploaded, std::move(uploaded));
                server_.record(connection_, Kind::DataEnded, endReason(error));
                co_return true;
            }

            asio::awaitable<bool> perform(DataPlan::Close const&)
            {
                co_await closeTransport();
                co_return false;
            }

            asio::awaitable<bool> perform(DataPlan::Reset const&)
            {
                closed_ = true;
                transport_->reset();
                co_return false;
            }

            asio::awaitable<bool> perform(DataPlan::StallAfter const& step)
            {
                if (const auto error = co_await transport_->writeAll(asio::buffer(step.bytes)); error)
                {
                    server_.record(connection_, Kind::DataEnded, endReason(error));
                    co_return false;
                }
                std::array<char, 4096u> discarded{};
                error_code error{};
                while (!error)
                {
                    const auto [readError, size] = co_await transport_->readSome(asio::buffer(discarded));
                    error = readError;
                }
                if (!stopped_)
                    server_.record(connection_, Kind::DataEnded, endReason(error));
                co_return false;
            }

            asio::awaitable<bool> perform(DataPlan::Hold const&)
            {
                co_await stoppedSignal_.wait();
                co_return false;
            }

          private:
            Server& server_;
            const std::size_t connection_;
            const DataPlan plan_;
            std::vector<DataPlan::Action> actions_;
            const asio::ip::address controlPeer_;
            tcp::acceptor acceptor_;
            std::optional<Transport> transport_{};
            Event begun_;
            Event finished_;
            Event stoppedSignal_;
            bool stopped_{false};
            bool closed_{false};
        };

        /**
         * @brief Runs a Script on one control connection.
         */
        class ControlSession : public std::enable_shared_from_this<ControlSession>
        {
          public:
            ControlSession(Server& server, std::size_t connection, Script script, tcp::socket socket)
                : server_{server}
                , connection_{connection}
                , script_{std::move(script)}
                , transport_{std::move(socket)}
            {}

            asio::awaitable<void> run()
            {
                const auto self = shared_from_this();
                error_code endpointError;
                peer_ = transport_.socket().remote_endpoint(endpointError).address();
                local_ = transport_.socket().local_endpoint(endpointError).address();
                server_.record(connection_, Kind::Connected, peer_.to_string());

                bool goesOn = true;
                if (server_.implicitTls())
                {
                    const auto outcome = co_await secure(server_, connection_, transport_, TlsBehaviour{}, "control");
                    if (outcome == TlsOutcome::Failed)
                        goesOn = co_await endAfterFailedHandshake();
                }

                for (; goesOn && !silent_ && step_ < script_.steps.size(); ++step_)
                {
                    goesOn = co_await std::visit(
                        [this](auto const& step)
                        {
                            return perform(step);
                        },
                        script_.steps[step_]
                    );
                }
                server_.scriptEnded();

                if (goesOn && !closed_)
                    co_await readUntilDisconnect();
                if (!closed_)
                    co_await transport_.close(false);
                for (auto const& data : dataPlans_)
                    data->stop();
                for (auto const& data : dataPlans_)
                    co_await data->awaitEnd();
                server_.connectionEnded();
            }

          private:
            /**
             * @brief The commands so far against what the script wanted, like a diff.
             */
            std::string diff(std::string_view expected, std::string_view received) const
            {
                std::string text = fmt::format("Control connection {}, step {}:\n", connection_, step_);
                for (auto const& command : commands_)
                    text += fmt::format("  = {}\n", command);
                text += fmt::format("  - {}\n  + {}", expected, received);
                return text;
            }

            /**
             * @brief The next line without its CRLF, nothing when the connection ended.
             */
            asio::awaitable<std::optional<std::string>> readLine()
            {
                auto end = received_.find("\r\n");
                while (end == std::string::npos)
                {
                    std::array<char, 4096u> chunk{};
                    const auto [error, size] = co_await transport_.readSome(asio::buffer(chunk));
                    // Only the new bytes and a CR before them can hold the line end, long lines stay linear.
                    const auto searchFrom = received_.empty() ? std::size_t{0u} : received_.size() - 1u;
                    received_.append(chunk.data(), size);
                    // OpenSSL 4 reads nothing and reports no error on a stream that already failed.
                    if (error || size == 0u)
                    {
                        endedBy_ = error ? endReason(error) : std::string{"nothing read and no error"};
                        server_.record(connection_, Kind::Disconnected, endedBy_);
                        closed_ = true;
                        co_return std::nullopt;
                    }
                    end = received_.find("\r\n", searchFrom);
                }
                auto line = received_.substr(0u, end);
                received_.erase(0u, end + 2u);
                server_.record(connection_, Kind::Command, line);
                commands_.push_back(line);
                co_return line;
            }

            /**
             * @brief Waits for the end of the connection, a command fails the script unless it stalls.
             */
            asio::awaitable<void> readUntilDisconnect()
            {
                while (const auto line = co_await readLine())
                {
                    if (!silent_)
                        server_.fail(diff("the end of the connection", fmt::format("command {}", *line)));
                }
            }

            asio::awaitable<bool> perform(Script::Expect const& step)
            {
                const auto expected = step.argumentPattern
                    ? fmt::format("command {} {}", step.verb, *step.argumentPattern)
                    : fmt::format("command {}", step.verb);
                const auto line = co_await readLine();
                if (!line)
                {
                    server_.fail(diff(expected, fmt::format("the connection ended: {}", endedBy_)));
                    co_return false;
                }

                const auto space = line->find(' ');
                lastVerb_ = line->substr(0u, space);
                const auto argument = space == std::string::npos ? std::string{} : line->substr(space + 1u);
                if (lastVerb_ != step.verb ||
                    (step.argumentPattern && !std::regex_match(argument, std::regex{*step.argumentPattern})))
                {
                    commands_.pop_back();
                    server_.fail(diff(expected, fmt::format("command {}", *line)));
                    co_return false;
                }
                if (renegotiationRequested_ && !transport_.renegotiationPending())
                {
                    renegotiationRequested_ = false;
                    server_.record(connection_, Kind::TlsEstablished, "control renegotiated");
                }
                co_return true;
            }

            asio::awaitable<bool> perform(Script::Send const& step)
            {
                co_await transport_.writeAll(asio::buffer(step.bytes));
                co_return true;
            }

            asio::awaitable<bool> perform(Script::Delay const& step)
            {
                asio::steady_timer timer{server_.executor(), step.duration};
                co_await timer.async_wait(noThrow);
                co_return true;
            }

            asio::awaitable<bool> perform(Script::Dribble const& step)
            {
                // Stops at the first byte the client did not take, the next read sees how the connection ended.
                asio::steady_timer timer{server_.executor()};
                error_code error{};
                std::size_t position = 0u;
                while (!error && position < step.bytes.size())
                {
                    error = co_await transport_.writeAll(asio::buffer(&step.bytes[position++], 1u));
                    timer.expires_after(step.interval);
                    co_await timer.async_wait(noThrow);
                }
                co_return true;
            }

            asio::awaitable<bool> perform(Script::CloseControl const&)
            {
                closed_ = true;
                server_.record(connection_, Kind::Disconnected, "closed by the script");
                co_await transport_.close(closeNotify_);
                co_return true;
            }

            asio::awaitable<bool> perform(Script::ResetControl const&)
            {
                closed_ = true;
                server_.record(connection_, Kind::Disconnected, "reset by the script");
                transport_.reset();
                co_return true;
            }

            asio::awaitable<bool> perform(Script::Stall const&)
            {
                silent_ = true;
                if (const auto behind = script_.steps.size() - step_ - 1u; behind > 0u)
                    server_.fail(diff("stall() as the last step", fmt::format("{} steps behind it", behind)));
                co_return true;
            }

            asio::awaitable<bool> perform(Script::ExpectDisconnect const&)
            {
                co_await readUntilDisconnect();
                co_return true;
            }

            asio::awaitable<bool> perform(Script::StartTls const& step)
            {
                closeNotify_ = !step.behaviour.closeWithoutCloseNotify;
                const auto outcome = co_await secure(server_, connection_, transport_, step.behaviour, "control");
                if (outcome != TlsOutcome::Failed)
                    co_return true;
                co_return co_await endAfterFailedHandshake();
            }

            /**
             * @brief Ends the connection after its TLS handshake failed: such a stream reads nothing more, so the
             * script ends with the connection.
             *
             * @return Whether the script goes on, which it never does.
             */
            asio::awaitable<bool> endAfterFailedHandshake()
            {
                closed_ = true;
                server_.record(connection_, Kind::Disconnected, "the TLS handshake failed");
                co_await transport_.close(false);
                co_return false;
            }

            asio::awaitable<bool> perform(Script::RenegotiateTls const&)
            {
                if (!co_await transport_.requestRenegotiation())
                {
                    server_.fail(diff("a renegotiation", "no HelloRequest could be sent"));
                    co_return false;
                }
                renegotiationRequested_ = true;
                co_return true;
            }

            asio::awaitable<bool> perform(Script::Checkpoint const& step)
            {
                server_.reachCheckpoint(step.name);
                co_return true;
            }

            asio::awaitable<bool> perform(Script::AwaitRelease const& step)
            {
                // release() posts the cancel, so it cannot come between the check and the wait.
                const auto waiter =
                    std::make_shared<asio::steady_timer>(server_.executor(), asio::steady_timer::time_point::max());
                server_.wakeOnRelease(waiter);
                while (!server_.takeRelease(step.name))
                {
                    waiter->expires_at(asio::steady_timer::time_point::max());
                    co_await waiter->async_wait(noThrow);
                }
                server_.stopWakingOnRelease(waiter);
                co_return true;
            }

            std::string passiveReply(DataPlan const& plan, std::uint16_t port) const
            {
                const auto announcedPort = plan.announcedPort.value_or(port);
                if (plan.announcedReply)
                {
                    return fmt::format(
                        "{}\r\n",
                        fmt::format(
                            fmt::runtime(*plan.announcedReply),
                            fmt::arg("port", port),
                            fmt::arg("high", port / 256u),
                            fmt::arg("low", port % 256u)
                        )
                    );
                }
                if (lastVerb_ == "EPSV")
                    return fmt::format("229 Entering Extended Passive Mode (|||{}|)\r\n", announcedPort);
                auto address = plan.announcedAddress.value_or(local_.to_string());
                std::ranges::replace(address, '.', ',');
                return fmt::format(
                    "227 Entering Passive Mode ({},{},{}).\r\n", address, announcedPort / 256u, announcedPort % 256u
                );
            }

            asio::awaitable<bool> perform(Script::Passive const& step)
            {
                if (lastVerb_ != "PASV" && lastVerb_ != "EPSV")
                {
                    server_.fail(diff("command PASV or EPSV before passive()", fmt::format("command {}", lastVerb_)));
                    co_return false;
                }
                auto data = std::make_shared<DataSession>(server_, connection_, step.plan, peer_);
                const auto port = data->listen(local_);
                if (!port)
                {
                    server_.fail(diff("a data port", port.error()));
                    co_return false;
                }
                const auto reply = passiveReply(step.plan, *port);
                co_await transport_.writeAll(asio::buffer(reply));
                dataPlans_.push_back(data);
                asio::co_spawn(
                    server_.executor(),
                    data->run(),
                    [server = &server_](std::exception_ptr exception)
                    {
                        if (exception)
                            server->fail("A data plan ended with an exception");
                    }
                );
                co_return true;
            }

            asio::awaitable<bool> perform(Script::BeginData const& step)
            {
                if (dataPlans_.empty())
                {
                    server_.fail(diff("a passive() before beginData()", "none"));
                    co_return false;
                }
                dataPlans_.back()->begin(step.actions);
                co_return true;
            }

            asio::awaitable<bool> perform(Script::AwaitData const&)
            {
                if (dataPlans_.empty())
                {
                    server_.fail(diff("a passive() before awaitData()", "none"));
                    co_return false;
                }
                co_await dataPlans_.back()->awaitEnd();
                co_return true;
            }

            asio::awaitable<bool> perform(Script::StopData const&)
            {
                if (dataPlans_.empty())
                {
                    server_.fail(diff("a passive() before stopData()", "none"));
                    co_return false;
                }
                dataPlans_.back()->stop();
                co_return true;
            }

            asio::awaitable<bool> perform(Script::Serve const& step)
            {
                if (const auto behind = script_.steps.size() - step_ - 1u; behind > 0u)
                {
                    server_.fail(diff("serve() as the last step", fmt::format("{} steps behind it", behind)));
                    co_return false;
                }
                while (const auto line = co_await readLine())
                {
                    lastVerb_ = line->substr(0u, line->find(' '));
                    const auto answer = step.responder->answer(*line);
                    for (auto const& answerStep : answer.steps)
                    {
                        if (std::holds_alternative<Script::Expect>(answerStep))
                        {
                            server_.fail(diff("an answer", "an expect() among the steps of an answer"));
                            co_return false;
                        }
                        const auto goesOn = co_await std::visit(
                            [this](auto const& nested)
                            {
                                return perform(nested);
                            },
                            answerStep
                        );
                        if (!goesOn || closed_ || silent_)
                            co_return false;
                    }
                }
                co_return false;
            }

          private:
            Server& server_;
            const std::size_t connection_;
            const Script script_;
            std::size_t step_{0u};
            Transport transport_;
            asio::ip::address peer_{};
            asio::ip::address local_{};
            std::string received_{};
            std::vector<std::string> commands_{};
            std::string lastVerb_{};
            std::string endedBy_{};
            /** @brief Every passive() so far, the last one is what beginData() and its kin act on. */
            std::vector<std::shared_ptr<DataSession>> dataPlans_{};
            bool closeNotify_{true};
            bool renegotiationRequested_{false};
            bool closed_{false};
            bool silent_{false};
        };
    }

    std::expected<void, std::string> ScriptedFtpServer::Implementation::open()
    {
        try
        {
            context_ = makeContext(certificate_);
        }
        catch (boost::system::system_error const& error)
        {
            return std::unexpected(fmt::format("Creating the TLS context: {}", error.what()));
        }

        const tcp::endpoint endpoint{asio::ip::make_address(address()), 0u};
        if (const auto error = openAcceptor(acceptor_, endpoint); error)
            return std::unexpected(fmt::format("Binding {}: {}", address(), error.message()));
        port_ = acceptor_.local_endpoint().port();

        asio::co_spawn(
            ioContext_,
            acceptConnections(),
            [this](std::exception_ptr exception)
            {
                if (exception)
                    fail("The accept loop ended with an exception");
            }
        );
        thread_ = std::thread{[this]()
            {
                ioContext_.run();
            }};
        return {};
    }

    asio::awaitable<void> ScriptedFtpServer::Implementation::acceptConnections()
    {
        error_code error{};
        while (!error)
        {
            auto [acceptError, socket] = co_await acceptor_.async_accept(noThrow);
            error = acceptError;
            if (!error)
                startSession(std::move(socket));
        }
    }

    void ScriptedFtpServer::Implementation::startSession(tcp::socket socket)
    {
        std::optional<Script> script{};
        std::size_t connection = 0u;
        {
            std::lock_guard lock{mutex_};
            connection = nextConnection_++;
            if (!scripts_.empty())
            {
                script = std::move(scripts_.front());
                scripts_.pop_front();
            }
        }
        if (!script)
        {
            fail(fmt::format("Control connection {} has no script", connection));
            connectionEnded();
            return;
        }

        auto session = std::make_shared<ControlSession>(*this, connection, std::move(*script), std::move(socket));
        asio::co_spawn(
            ioContext_,
            session->run(),
            [this, connection](std::exception_ptr exception)
            {
                if (exception)
                {
                    fail(fmt::format("Control connection {} ended with an exception", connection));
                    scriptEnded();
                    connectionEnded();
                }
            }
        );
    }

    ScriptedFtpServer::ScriptedFtpServer(std::unique_ptr<Implementation> implementation)
        : impl_{std::move(implementation)}
    {}
    ScriptedFtpServer::~ScriptedFtpServer() = default;

    std::expected<std::unique_ptr<ScriptedFtpServer>, std::string> ScriptedFtpServer::start(Options options)
    {
        return testPki().and_then(
            [&options](
                std::reference_wrapper<TestPki const> pki
            ) -> std::expected<std::unique_ptr<ScriptedFtpServer>, std::string>
            {
                const auto certificate = options.certificate.value_or(pki.get().server);
                auto implementation = std::make_unique<Implementation>(std::move(options), certificate);
                if (const auto opened = implementation->open(); !opened)
                    return std::unexpected(opened.error());
                return std::unique_ptr<ScriptedFtpServer>{new ScriptedFtpServer{std::move(implementation)}};
            }
        );
    }

    std::string ScriptedFtpServer::address() const
    {
        return impl_->address();
    }
    std::uint16_t ScriptedFtpServer::port() const
    {
        return impl_->port();
    }
    void ScriptedFtpServer::addConnection(Script script)
    {
        impl_->addConnection(std::move(script));
    }
    bool ScriptedFtpServer::waitForCheckpoint(std::string const& name, std::chrono::milliseconds timeout)
    {
        return impl_->waitForCheckpoint(name, timeout);
    }
    void ScriptedFtpServer::release(std::string name)
    {
        impl_->release(std::move(name));
    }

    bool ScriptedFtpServer::waitForTranscriptEntry(TranscriptEntry::Kind kind, std::chrono::milliseconds timeout)
    {
        return impl_->waitForTranscriptEntry(kind, timeout);
    }
    bool ScriptedFtpServer::waitUntilFinished(std::chrono::milliseconds timeout)
    {
        return impl_->waitUntilFinished(timeout);
    }
    bool ScriptedFtpServer::waitUntilDisconnected(std::chrono::milliseconds timeout)
    {
        return impl_->waitUntilDisconnected(timeout);
    }
    std::vector<TranscriptEntry> ScriptedFtpServer::transcript() const
    {
        return impl_->transcript();
    }
    std::vector<std::string> ScriptedFtpServer::commands(std::size_t connection) const
    {
        return impl_->commands(connection);
    }
    std::vector<std::string> ScriptedFtpServer::failures() const
    {
        return impl_->failures();
    }
}
