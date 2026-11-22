#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/data_connection.hpp>
#include <ftp/client/login.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/engine/resolver.hpp>
#include <ftp/pool/lease.hpp>
#include <ftp/pool/pooled_connection.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FileTransferProtocol
{
    struct PoolOptions
    {
        std::string host{};
        std::uint16_t port{21u};
        LoginOptions login{};
        ControlConnectionOptions control{};
        /** @brief Connections for transfers beside C0, min(concurrency, maxConnections - 1) of the session. */
        std::size_t transferConnections{3u};
        std::chrono::milliseconds resolveTimeout{10'000};
        /** @brief Logins of C0 in the background after it was lost during use, before the session is Lost. */
        std::size_t reloginAttempts{3u};
        /** @brief The wait before the second background login, doubled for each one after it. */
        std::chrono::milliseconds reloginBackoff{500};
        /** @brief How long a refused login keeps the learned limit, then one more connection may be tried. */
        std::chrono::milliseconds connectionLimitBackoff{30'000};
        /** @brief An idle transfer connection ends with QUIT after this time. */
        std::chrono::milliseconds transferLinger{30'000};
        /** @brief Sends NOOP on an idle C0 every idleNoopInterval, so the server's idle timeout does not end it. */
        bool keepIdleAlive{false};
        std::chrono::milliseconds idleNoopInterval{60'000};
        std::chrono::milliseconds quitTimeout{1'000};
    };

    enum class PoolHealth : std::uint8_t
    {
        /** @brief C0 is logged in, or ended while idle and logs in again at its next use. */
        Healthy,
        /** @brief C0 was lost during use, logins run in the background. */
        Degraded,
        /** @brief Those logins gave up, a login was refused for good, or the start failed. */
        Lost,
    };

    /**
     * @brief The control connections of one FTP session: C0 for metadata, which commands take in turn, and up to
     *        PoolOptions::transferConnections more, one per transfer. Each connection logs in when it is first
     *        needed, with the endpoints the start looked up; they are looked up again once all of them failed.
     *
     * A refused login (ConnectionLimit) lowers the number of connections to those the session has, for
     * connectionLimitBackoff. With only C0 left, transfers take C0 when it is idle. Once that time is over the kept
     * wakes run, a transfer that waited may try another login.
     *
     * C0 lost during use makes the session Degraded: it logs in again in the background, a few times with a growing
     * wait, and waits for commands up to the connect timeout meanwhile. If that fails, the session is Lost and the
     * lost handler runs. C0 that ends while idle, at the server's idle timeout say, logs in again at its next use.
     * Credentials and the pin of the start are used again, nothing asks the user.
     *
     * Everything runs on the strand the pool is given, but the thread-safe health(), connectionLimit() and the release
     * of a Lease. Connections and their transfers run on strands of their own.
     */
    class Pool : public std::enable_shared_from_this<Pool>
    {
        struct Private
        {};

      public:
        using LeaseResult = std::expected<Lease, RemoteIo::RemoteError>;
        /** @brief Called once on the pool's strand when the session is lost, must not block. */
        using LostHandler = std::function<void(RemoteIo::RemoteError const&)>;
        /** @brief Called once, from the pool's strand, see acquireTransfer(). An exception of it loses the session. */
        using Wake = std::function<void()>;

        /**
         * @param strand The session strand, connections get strands of their own on its executor.
         * @return The handle of the owner, a weak_ptr of it ends with it. Logins in the background keep the pool
         *         itself while they run. Once the last copy of the handle is gone they end, the connections close
         *         without QUIT, and neither the lost handler nor a kept wake is called any more: at once when the
         *         handle goes on the pool's strand, else as soon as the strand gets to it.
         */
        static std::shared_ptr<Pool> create(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::shared_ptr<Resolver> resolver,
            PoolOptions options,
            LostHandler onLost = {}
        );

        Pool(
            Private,
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::shared_ptr<Resolver> resolver,
            PoolOptions options,
            LostHandler onLost
        );
        /**
         * @brief Closes the connections without QUIT.
         */
        ~Pool();
        Pool(Pool const&) = delete;
        Pool& operator=(Pool const&) = delete;
        Pool(Pool&&) = delete;
        Pool& operator=(Pool&&) = delete;

        boost::asio::strand<boost::asio::any_io_executor> const& strand() const;

        /**
         * @brief Looks up the server and logs in C0, once.
         *
         * @return What the login learned. The error of the lookup or of the login, ConnectionLost after a close()
         *         meanwhile, after which the pool is Lost without calling the lost handler. With TrustRequired
         *         untrustedCertificate() has the certificate.
         */
        boost::asio::awaitable<std::expected<LoginResult, RemoteIo::RemoteError>> start();

        /**
         * @brief A connection for commands: C0 when it is free, else an idle transfer connection, else C0 in turn.
         *        C0 that ended while idle logs in again first, one attempt; while Degraded the wait for C0 ends after
         *        the connect timeout.
         *
         * @param held A connection the caller holds a lease of itself. Where the turn would be for that one, only
         *        the caller could end the wait, so the result is Busy at once.
         * @return The lease. ConnectionLost when the session is Lost or closing, the error of a failed login of C0,
         *         TimedOut after the wait while Degraded, Busy as for @p held, Cancelled.
         */
        boost::asio::awaitable<LeaseResult> acquireCommand(std::shared_ptr<ControlConnection> held = nullptr);

        /**
         * @brief A connection for one transfer: an idle transfer connection, a new login while the limit allows one,
         *        C0 when the limit leaves only C0 and C0 is free. Never waits for a release.
         *
         * @param wakeWhenFree Kept when the result is Busy and called once a connection is free or a lowered limit
         *        may be tried again, then open again.
         * @return The lease. Busy when every connection is in use, the error of a failed login, ConnectionLost when
         *         the session is Lost or closing, Cancelled.
         */
        boost::asio::awaitable<LeaseResult> acquireTransfer(Wake wakeWhenFree = {});

        /**
         * @brief Ends the session: QUIT on the idle connections at once, each within the quit timeout, and a close of
         *        those in use. Waits for C0 fail, the kept wakes run, acquires fail from now on.
         */
        boost::asio::awaitable<void> close();

        /**
         * @brief The certificate the last login refused with TrustRequired, what a trust prompt shows.
         */
        std::optional<CertificateInfo> const& untrustedCertificate() const;

        /**
         * @brief Thread-safe.
         */
        PoolHealth health() const;

        /**
         * @brief How many connections the session may have now, C0 included. Thread-safe.
         */
        std::size_t connectionLimit() const;

      private:
        friend class Lease;
        struct Waiter;
        using ConnectionResult = std::expected<std::shared_ptr<PooledConnection>, RemoteIo::RemoteError>;

        std::shared_ptr<PooledConnection> makeConnection(std::string name);
        boost::asio::awaitable<ResolveResult> lookUp();
        boost::asio::awaitable<ConnectionResult> logInConnection(bool metadata);
        boost::asio::awaitable<ConnectionResult> logInAs(std::string name);
        /**
         * @return Nothing after a refusal that lowered the limit.
         */
        boost::asio::awaitable<std::optional<LeaseResult>> logInTransfer();
        /**
         * @return The lease of @p connection, nothing when it ended meanwhile, which drops it.
         */
        boost::asio::awaitable<std::optional<LeaseResult>> leaseIfOpen(std::shared_ptr<PooledConnection> connection);
        boost::asio::awaitable<LeaseResult> waitForMetadata();
        boost::asio::awaitable<void> logInMetadataOnce();
        boost::asio::awaitable<void> logInMetadataInBackground(RemoteIo::RemoteError failure);
        boost::asio::awaitable<void> sendNoop(std::shared_ptr<PooledConnection> connection);
        boost::asio::awaitable<void> quitAll(std::vector<std::shared_ptr<PooledConnection>> connections);
        void spawn(boost::asio::awaitable<void> work);

        std::optional<RemoteIo::RemoteError> refusal() const;
        void reserve(std::shared_ptr<PooledConnection> const& connection);
        Lease lease(std::shared_ptr<PooledConnection> const& connection);
        std::shared_ptr<PooledConnection> idleTransfer() const;
        bool metadataFree() const;
        std::size_t transferCapacity();
        void learnLimit();
        /**
         * @brief The time of a lowered limit is over, transfers that wait may try another login.
         */
        void endLimit();
        /**
         * @brief Logs in C0 for the commands that need it now, one attempt. Does nothing while C0 is there, a login
         *        of it runs or the session is not Healthy.
         */
        void connectMetadataIfGone();
        void adoptMetadata(std::shared_ptr<PooledConnection> connection);
        void promote(std::shared_ptr<PooledConnection> const& connection);
        bool grantMetadata();
        void failCommandWaiters(RemoteIo::RemoteError const& error);
        /**
         * @brief Calls the kept wakes, each once. One that throws loses the session, the wakes behind it still run.
         */
        void fireWakes();
        void wakeThrew(std::exception_ptr exception);
        /**
         * @brief The owner let go of its handle: ends the work in the background and calls nothing of the owner
         *        any more.
         */
        void abandon();
        void closeConnections();
        void degrade(RemoteIo::RemoteError const& error);
        void loseSession(RemoteIo::RemoteError error);
        void release(std::shared_ptr<PooledConnection> const& connection, bool open);
        void takeEnd(std::shared_ptr<PooledConnection> const& connection, RemoteIo::RemoteError const& error);
        void lose(std::shared_ptr<PooledConnection> const& connection, RemoteIo::RemoteError const& error, bool inUse);
        void drop(std::shared_ptr<PooledConnection> const& connection);
        /**
         * @brief A connection is free: C0 goes to the next in turn, a transfer connection lingers.
         */
        void makeAvailable(std::shared_ptr<PooledConnection> const& connection);
        void armIdleTimer(std::shared_ptr<PooledConnection> const& connection, std::chrono::milliseconds duration);
        void endIdleTime(std::shared_ptr<PooledConnection> const& connection);

        boost::asio::strand<boost::asio::any_io_executor> strand_;
        std::shared_ptr<Resolver> resolver_;
        PoolOptions options_;
        LostHandler onLost_;
        std::shared_ptr<PassiveChoice> passiveChoice_;
        Endpoints endpoints_{};
        std::shared_ptr<PooledConnection> metadata_{};
        std::vector<std::shared_ptr<PooledConnection>> transfers_{};
        /** @brief Connections whose login runs, a close ends them. */
        std::vector<std::shared_ptr<PooledConnection>> connecting_{};
        std::size_t transfersConnecting_{0u};
        std::deque<std::shared_ptr<Waiter>> commandWaiters_{};
        std::vector<Wake> wakes_{};
        boost::asio::steady_timer reloginTimer_;
        /** @brief Runs out with limitedUntil_. */
        boost::asio::steady_timer limitTimer_;
        std::uint64_t transferNumber_{0u};
        std::optional<std::chrono::steady_clock::time_point> limitedUntil_{};
        std::optional<RemoteIo::RemoteError> lostReason_{};
        std::optional<CertificateInfo> untrustedCertificate_{};
        std::atomic<PoolHealth> health_{PoolHealth::Healthy};
        std::atomic<std::size_t> connectionLimit_;
        bool started_{false};
        bool closed_{false};
        /** @brief The owner is gone, see abandon(). */
        bool abandoned_{false};
        /** @brief A login of C0 runs, as part of a command or in the background. */
        bool metadataConnecting_{false};
        /** @brief The server refused C0 while transfer connections held its slots, the next one released becomes C0. */
        bool metadataFromNextRelease_{false};
    };
}
