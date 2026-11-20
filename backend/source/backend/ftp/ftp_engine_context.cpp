#include <backend/ftp/ftp_engine_context.hpp>
#include <log/log.hpp>
#include <utility/error_message.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cassert>
#include <exception>
#include <thread>
#include <variant>

namespace
{
    /**
     * @brief One engine thread on an io_context. An exception that escapes a handler is logged and the thread runs on.
     */
    class SingleEngineThread
    {
      public:
        SingleEngineThread()
            : thread_{[this]()
                  {
                      run();
                  }}
        {}
        ~SingleEngineThread()
        {
            workGuard_.reset();
            context_.stop();
            thread_.join();
        }
        SingleEngineThread(SingleEngineThread const&) = delete;
        SingleEngineThread& operator=(SingleEngineThread const&) = delete;
        SingleEngineThread(SingleEngineThread&&) = delete;
        SingleEngineThread& operator=(SingleEngineThread&&) = delete;

        boost::asio::any_io_executor executor()
        {
            return context_.get_executor();
        }

        bool runningInThisThread()
        {
            return context_.get_executor().running_in_this_thread();
        }

      private:
        void run()
        {
            while (!context_.stopped())
                runUntilStoppedOrThrown();
        }

        void runUntilStoppedOrThrown()
        {
            try
            {
                context_.run();
            }
            catch (std::exception const& exception)
            {
                Log::error(
                    "FTP engine: A handler threw, the engine thread goes on: {}", Utility::errorMessage(exception)
                );
            }
            catch (...)
            {
                Log::error("FTP engine: A handler threw something that is no exception, the engine thread goes on.");
            }
        }

        boost::asio::io_context context_{1};
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard_{context_.get_executor()};
        std::thread thread_;
    };

    using EngineThreads = std::variant<SingleEngineThread, boost::asio::thread_pool>;

    EngineThreads makeEngineThreads(std::size_t count)
    {
        if (count > 1u)
            return EngineThreads{std::in_place_type<boost::asio::thread_pool>, count};
        return EngineThreads{std::in_place_type<SingleEngineThread>};
    }

    boost::asio::any_io_executor executorOf(SingleEngineThread& thread)
    {
        return thread.executor();
    }

    boost::asio::any_io_executor executorOf(boost::asio::thread_pool& pool)
    {
        return pool.get_executor();
    }

    bool runsOn(SingleEngineThread& thread)
    {
        return thread.runningInThisThread();
    }

    bool runsOn(boost::asio::thread_pool& pool)
    {
        return pool.get_executor().running_in_this_thread();
    }
}

struct FtpEngineContext::Implementation
{
    explicit Implementation(std::size_t count)
        : threadCount{count}
        , threads{makeEngineThreads(count)}
    {}

    ~Implementation()
    {
        assert(!runningInThisThread());
    }

    Implementation(Implementation const&) = delete;
    Implementation& operator=(Implementation const&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;

    bool runningInThisThread()
    {
        return std::visit(
            [](auto& engineThreads)
            {
                return runsOn(engineThreads);
            },
            threads
        );
    }

    const std::size_t threadCount;
    EngineThreads threads;
    /** @brief After the threads, so it goes first: it ends the lookups still waiting on them as Cancelled. */
    std::shared_ptr<FileTransferProtocol::Resolver> resolver = std::make_shared<FileTransferProtocol::Resolver>();
};

FtpEngineContext::FtpEngineContext(std::size_t threads)
    : impl_{std::make_unique<Implementation>(threads)}
{
    assert(threads >= 1u);
    Log::info("FTP engine: Started with {} thread(s).", threads);
}

FtpEngineContext::~FtpEngineContext()
{
    if (impl_->runningInThisThread())
        Log::error("FTP engine: Destroyed on one of its own threads, which cannot join themselves.");
}

boost::asio::any_io_executor FtpEngineContext::executor() const
{
    return std::visit(
        [](auto& engineThreads)
        {
            return executorOf(engineThreads);
        },
        impl_->threads
    );
}

std::shared_ptr<FileTransferProtocol::Resolver> FtpEngineContext::resolver() const
{
    return impl_->resolver;
}

std::size_t FtpEngineContext::threads() const
{
    return impl_->threadCount;
}

bool FtpEngineContext::runningInThisThread() const
{
    return impl_->runningInThisThread();
}
