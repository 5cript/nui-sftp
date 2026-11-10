#include <ftp/engine/engine.hpp>
#include <log/log.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/co_spawn.hpp>
#include <fmt/format.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using RemoteIo::RemoteErrorKind;
        using Detail::errorOf;

        std::string describe(std::exception_ptr exception)
        {
            try
            {
                std::rethrow_exception(exception);
            }
            catch (std::exception const& error)
            {
                return error.what();
            }
            catch (...)
            {
                return "an exception of an unknown type";
            }
        }
    }

    std::shared_ptr<Engine> Engine::create(
        asio::any_io_executor executor,
        std::shared_ptr<Resolver> resolver,
        std::shared_ptr<TaskGate> tasks,
        EngineOptions options,
        LostHandler onLost
    )
    {
        auto engine = std::make_shared<Engine>(Private{}, std::move(executor), std::move(tasks), std::move(onLost));
        engine->pool_ = Pool::create(
            engine->strand_,
            std::move(resolver),
            std::move(options.pool),
            [weak = std::weak_ptr{engine}](RemoteIo::RemoteError const& error)
            {
                if (const auto locked = weak.lock())
                    locked->reportLost(error);
            }
        );
        return engine;
    }

    Engine::Engine(Private, asio::any_io_executor executor, std::shared_ptr<TaskGate> tasks, LostHandler onLost)
        : strand_{asio::make_strand(std::move(executor))}
        , tasks_{std::move(tasks)}
        , onLost_{std::move(onLost)}
        , work_{std::make_shared<WorkCounter>()}
    {}

    Engine::~Engine() = default;

    Engine::Strand const& Engine::strand() const
    {
        return strand_;
    }

    std::shared_ptr<Pool> Engine::pool() const
    {
        if (!pool_)
            return nullptr;
        return pool_->shared_from_this();
    }

    bool Engine::spawn(asio::awaitable<void> coroutine)
    {
        std::scoped_lock lock{postMutex_};
        if (postsRefused_)
            return false;
        asio::co_spawn(
            strand_,
            std::move(coroutine),
            [self = shared_from_this(), token = work()](std::exception_ptr exception)
            {
                if (exception)
                    self->handlerThrew(exception);
            }
        );
        return true;
    }

    WorkCounter::Token Engine::work()
    {
        return work_->acquire();
    }

    bool Engine::pushTask(std::function<void()> task)
    {
        return tasks_->push(std::move(task));
    }

    bool Engine::onProcessingThread() const
    {
        return tasks_->onProcessingThread();
    }

    void Engine::setStopping()
    {
        stopping_ = true;
    }

    bool Engine::stopping() const
    {
        return stopping_;
    }

    bool Engine::failed() const
    {
        return failed_;
    }

    void Engine::refusePosts()
    {
        std::scoped_lock lock{postMutex_};
        postsRefused_ = true;
    }

    bool Engine::waitIdle(std::chrono::milliseconds limit)
    {
        return work_->waitIdle(limit);
    }

    void Engine::handlerThrew(std::exception_ptr exception)
    {
        if (strand_.running_in_this_thread())
        {
            fail(exception);
            return;
        }
        // Also while posts are refused: the failure must reach the session strand.
        asio::post(
            strand_,
            [self = shared_from_this(), token = work(), exception]()
            {
                self->fail(exception);
            }
        );
    }

    void Engine::fail(std::exception_ptr exception)
    {
        if (failed_.exchange(true))
            return;
        const auto what = describe(exception);
        Log::error("FTP: A handler of the session threw, the session fails: {}", what);
        // The pool's end closes every connection, and ends the waits and the logins in the background.
        pool_.reset();
        reportLost(errorOf(RemoteErrorKind::ConnectionLost, fmt::format("The FTP session failed: {}", what)));
    }

    void Engine::reportLost(RemoteIo::RemoteError error)
    {
        if (!onLost_ || lostReported_.exchange(true))
            return;
        pushTask(
            [handler = onLost_, error = std::move(error)]()
            {
                handler(error);
            }
        );
    }
}
