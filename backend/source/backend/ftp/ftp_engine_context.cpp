#include <backend/ftp/ftp_engine_context.hpp>
#include <log/log.hpp>
#include <utility/error_message.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <cassert>
#include <exception>
#include <thread>
#include <vector>

namespace
{
    /**
     * @brief The engine threads on one io_context. An exception that escapes a handler is logged and ends the
     *        application: the operation of that handler is lost and what it left half done is not to be trusted.
     */
    class EngineThreads
    {
      public:
        explicit EngineThreads(std::size_t count)
            : context_{static_cast<int>(count)}
        {
            threads_.reserve(count);
            for (std::size_t index = 0u; index < count; ++index)
            {
                threads_.emplace_back(
                    [this]()
                    {
                        run();
                    }
                );
            }
        }
        ~EngineThreads()
        {
            workGuard_.reset();
            context_.stop();
            for (auto& thread : threads_)
                thread.join();
        }
        EngineThreads(EngineThreads const&) = delete;
        EngineThreads& operator=(EngineThreads const&) = delete;
        EngineThreads(EngineThreads&&) = delete;
        EngineThreads& operator=(EngineThreads&&) = delete;

        boost::asio::any_io_executor executor()
        {
            return context_.get_executor();
        }

        bool runningInThisThread()
        {
            return context_.get_executor().running_in_this_thread();
        }

      private:
        void run() noexcept
        {
            try
            {
                context_.run();
            }
            catch (std::exception const& exception)
            {
                Log::critical(
                    "FTP engine: A handler threw, the application ends: {}", Utility::errorMessage(exception)
                );
                std::terminate();
            }
            catch (...)
            {
                Log::critical("FTP engine: A handler threw something that is no exception, the application ends.");
                std::terminate();
            }
        }

        boost::asio::io_context context_;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard_{context_.get_executor()};
        std::vector<std::thread> threads_{};
    };
}

struct FtpEngineContext::Implementation
{
    explicit Implementation(std::size_t count)
        : threadCount{count}
        , threads{count}
    {}

    ~Implementation()
    {
        assert(!threads.runningInThisThread());
    }

    Implementation(Implementation const&) = delete;
    Implementation& operator=(Implementation const&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;

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
    if (impl_->threads.runningInThisThread())
        Log::error("FTP engine: Destroyed on one of its own threads, which cannot join themselves.");
}

boost::asio::any_io_executor FtpEngineContext::executor() const
{
    return impl_->threads.executor();
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
    return impl_->threads.runningInThisThread();
}
