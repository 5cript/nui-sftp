#include <ftp/engine/resolver.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using RemoteIo::RemoteErrorKind;
        using Detail::errorOf;

        RemoteIo::RemoteError stopped()
        {
            return errorOf(RemoteErrorKind::Cancelled, "The resolver stopped before the lookup ended");
        }

        /**
         * @brief One lookup and the wait for it, completed once: by its result, a cancel or the end of the resolver.
         *        Thread-safe.
         */
        class Job : public std::enable_shared_from_this<Job>
        {
          public:
            Job(std::string host, std::uint16_t port)
                : host_{std::move(host)}
                , port_{port}
            {}

            /**
             * @brief Takes the handler and connects its cancellation slot, on the thread that started the lookup.
             */
            void arm(Resolver::Handler handler, asio::any_io_executor const& handlerExecutor)
            {
                auto slot = asio::get_associated_cancellation_slot(handler);
                auto executor = asio::prefer(handlerExecutor, asio::execution::outstanding_work.tracked);
                {
                    std::scoped_lock lock{mutex_};
                    handler_.emplace(std::move(handler));
                    executor_ = std::move(executor);
                    slot_ = slot;
                }
                if (!slot.is_connected())
                    return;
                slot.assign(
                    [weak = weak_from_this()](asio::cancellation_type type)
                    {
                        if (type == asio::cancellation_type::none)
                            return;
                        if (const auto job = weak.lock())
                            job->complete(
                                std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The lookup was cancelled"))
                            );
                    }
                );
            }

            /**
             * @brief Posts @p result to the handler's executor, unless the job completed already.
             */
            void complete(ResolveResult result)
            {
                std::optional<Resolver::Handler> handler{};
                asio::any_io_executor executor{};
                asio::cancellation_slot slot{};
                {
                    std::scoped_lock lock{mutex_};
                    if (!handler_)
                        return;
                    handler = std::exchange(handler_, std::nullopt);
                    executor = std::move(executor_);
                    slot = slot_;
                }
                // The slot belongs to the handler's executor, and a cancel may be running its handler right now.
                asio::post(
                    executor,
                    [handler = std::move(*handler), slot, result = std::move(result)]() mutable
                    {
                        if (slot.is_connected())
                            slot.clear();
                        std::move(handler)(std::move(result));
                    }
                );
            }

            bool completed() const
            {
                std::scoped_lock lock{mutex_};
                return !handler_;
            }

            std::string const& host() const
            {
                return host_;
            }

            std::uint16_t port() const
            {
                return port_;
            }

          private:
            const std::string host_;
            const std::uint16_t port_;
            mutable std::mutex mutex_{};
            std::optional<Resolver::Handler> handler_{};
            asio::any_io_executor executor_{};
            asio::cancellation_slot slot_{};
        };
    }

    struct Resolver::Shared
    {
        Shared(Lookup lookupFunction, std::chrono::milliseconds stopWaitTime, std::size_t threads)
            : lookup{std::move(lookupFunction)}
            , stopWait{stopWaitTime}
            , running(threads)
            , ended(threads, false)
        {}

        const Lookup lookup;
        const std::chrono::milliseconds stopWait;
        std::mutex mutex{};
        std::condition_variable workAvailable{};
        std::condition_variable threadEnded{};
        std::deque<std::shared_ptr<Job>> queued{};
        /** @brief The job each thread looks up, by the index of the thread. */
        std::vector<std::shared_ptr<Job>> running;
        std::vector<bool> ended;
        bool stopping{false};
    };

    Resolver::Resolver(ResolverOptions options, Lookup lookup)
        : shared_{std::make_shared<Shared>(std::move(lookup), options.stopWait, options.threads)}
    {
        assert(options.threads > 0u && "a resolver without threads never resolves");
        threads_.reserve(options.threads);
        for (std::size_t index = 0u; index < options.threads; ++index)
            threads_.emplace_back(&Resolver::work, shared_, index);
    }

    Resolver::~Resolver()
    {
        std::vector<std::shared_ptr<Job>> open{};
        {
            std::scoped_lock lock{shared_->mutex};
            shared_->stopping = true;
            open.assign(
                std::make_move_iterator(shared_->queued.begin()), std::make_move_iterator(shared_->queued.end())
            );
            shared_->queued.clear();
            std::ranges::copy_if(
                shared_->running,
                std::back_inserter(open),
                [](auto const& job)
                {
                    return job != nullptr;
                }
            );
        }
        shared_->workAvailable.notify_all();
        for (auto const& job : open)
            job->complete(std::unexpected(stopped()));

        std::vector<bool> ended{};
        {
            std::unique_lock lock{shared_->mutex};
            shared_->threadEnded.wait_for(
                lock,
                shared_->stopWait,
                [this]()
                {
                    return std::ranges::all_of(
                        shared_->ended,
                        [](bool threadEnded)
                        {
                            return threadEnded;
                        }
                    );
                }
            );
            ended = shared_->ended;
        }
        for (std::size_t index = 0u; index < threads_.size(); ++index)
        {
            if (ended[index])
                threads_[index].join();
            else
                threads_[index].detach();
        }
    }

    void Resolver::start(
        std::shared_ptr<Shared> const& shared,
        std::string host,
        std::uint16_t port,
        Handler handler,
        asio::any_io_executor executor
    )
    {
        const auto job = std::make_shared<Job>(std::move(host), port);
        job->arm(std::move(handler), executor);
        bool accepted = false;
        {
            std::scoped_lock lock{shared->mutex};
            accepted = !shared->stopping;
            if (accepted)
                shared->queued.push_back(job);
        }
        if (!accepted)
        {
            job->complete(std::unexpected(stopped()));
            return;
        }
        shared->workAvailable.notify_one();
    }

    void Resolver::work(std::shared_ptr<Shared> shared, std::size_t index)
    {
        std::unique_lock lock{shared->mutex};
        while (!shared->stopping)
        {
            shared->workAvailable.wait(
                lock,
                [&shared]()
                {
                    return shared->stopping || !shared->queued.empty();
                }
            );
            if (shared->queued.empty())
                continue;
            auto job = std::move(shared->queued.front());
            shared->queued.pop_front();
            if (job->completed())
                continue;
            shared->running[index] = job;
            lock.unlock();
            job->complete(shared->lookup(job->host(), job->port()));
            lock.lock();
            shared->running[index].reset();
        }
        shared->ended[index] = true;
        shared->threadEnded.notify_all();
    }

    ResolveResult Resolver::systemLookup(std::string const& host, std::uint16_t port)
    {
        asio::io_context context{1};
        asio::ip::tcp::resolver resolver{context};
        boost::system::error_code error{};
        const auto results = resolver.resolve(
            host,
            std::to_string(port),
            asio::ip::resolver_base::numeric_service | asio::ip::resolver_base::address_configured,
            error
        );
        if (error)
            return std::unexpected(
                errorOf(RemoteErrorKind::NoConnection, fmt::format("Could not look up the server: {}", error.message()))
            );
        Endpoints endpoints{};
        for (auto const& entry : results)
            endpoints.push_back(entry.endpoint());
        if (endpoints.empty())
            return std::unexpected(errorOf(RemoteErrorKind::NoConnection, "The lookup found no address of the server"));
        return endpoints;
    }
}
