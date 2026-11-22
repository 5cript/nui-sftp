#pragma once

#include <remote_io/remote_error.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace FileTransferProtocol
{
    using Endpoints = std::vector<boost::asio::ip::tcp::endpoint>;
    using ResolveResult = std::expected<Endpoints, RemoteIo::RemoteError>;

    struct ResolverOptions
    {
        /** @brief Lookups that run at once, each on a thread of its own. A lookup that hangs holds its thread. */
        std::size_t threads{2u};
        /** @brief How long the destructor waits for its threads, one still in a lookup then runs on detached. */
        std::chrono::milliseconds stopWait{100};
    };

    /**
     * @brief Looks up the servers of every FTP session of the application on threads of its own. Asio's resolver
     *        serializes the lookups of an execution context on one thread, one that hangs would hold up every
     *        session on the engine's executor.
     *
     * A lookup completes on the executor of its handler. Its wait can be cancelled at any time, the result that
     * comes later is dropped, so a session never waits for getaddrinfo. Destroy the resolver after every lookup
     * completed or was cancelled: its destructor ends the waits still open as Cancelled and does not wait for a
     * thread stuck in a lookup, which owns all it touches and ends on its own.
     */
    class Resolver
    {
      public:
        /** @brief Called on a resolver thread, may block. */
        using Lookup = std::function<ResolveResult(std::string const& host, std::uint16_t port)>;
        using Handler = boost::asio::any_completion_handler<void(ResolveResult)>;

        explicit Resolver(ResolverOptions options = {}, Lookup lookup = systemLookup);
        ~Resolver();
        Resolver(Resolver const&) = delete;
        Resolver& operator=(Resolver const&) = delete;
        Resolver(Resolver&&) = delete;
        Resolver& operator=(Resolver&&) = delete;

        /**
         * @brief The addresses of @p host, a name or an address.
         *
         * @return NoConnection when the lookup failed or found nothing, Cancelled after a cancel of the wait or the
         *         end of the resolver.
         */
        template <typename CompletionTokenT>
        auto resolve(std::string host, std::uint16_t port, CompletionTokenT&& token)
        {
            return boost::asio::async_initiate<CompletionTokenT, void(ResolveResult)>(
                [shared = shared_](auto handler, std::string host, std::uint16_t port)
                {
                    // The erased handler's executor cannot take a post, the concrete one can.
                    boost::asio::any_io_executor executor = boost::asio::get_associated_executor(handler);
                    start(shared, std::move(host), port, Handler{std::move(handler)}, std::move(executor));
                },
                token,
                std::move(host),
                port
            );
        }

        /**
         * @brief getaddrinfo through Asio, on the calling thread.
         */
        static ResolveResult systemLookup(std::string const& host, std::uint16_t port);

      private:
        struct Shared;

        static void start(
            std::shared_ptr<Shared> const& shared,
            std::string host,
            std::uint16_t port,
            Handler handler,
            boost::asio::any_io_executor executor
        );
        static void work(std::shared_ptr<Shared> shared, std::size_t index);

        std::shared_ptr<Shared> shared_;
        std::vector<std::thread> threads_{};
    };
}
