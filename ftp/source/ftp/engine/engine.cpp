#include <ftp/engine/engine.hpp>
#include <log/log.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/co_spawn.hpp>
#include <fmt/format.h>

#include <cassert>
#include <future>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
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

    bool Engine::beginCall(std::shared_ptr<Detail::CallState> const& pending)
    {
        if (stopping() || failed())
        {
            pending->fail(closedError());
            return false;
        }
        pending->number = ++callNumber_;
        calls_.emplace(pending->number, pending);
        armCallTimer(pending, pending->limits().backstop);
        return true;
    }

    void Engine::armCallTimer(std::shared_ptr<Detail::CallState> const& pending, std::chrono::milliseconds duration)
    {
        pending->timer().expires_after(duration);
        pending->timer().async_wait(guarded(
            [self = shared_from_this(), pending](boost::system::error_code error)
            {
                if (!error)
                    self->callTimeUp(pending);
            }
        ));
    }

    void Engine::callTimeUp(std::shared_ptr<Detail::CallState> const& pending)
    {
        if (pending->completed())
            return;
        if (!pending->draining())
        {
            Log::warn(
                "FTP: An operation ran past its backstop of {} ms, it is cancelled", pending->limits().backstop.count()
            );
            pending->cancel();
            armCallTimer(pending, pending->limits().drain);
            return;
        }
        Log::warn(
            "FTP: An operation did not end {} ms after its cancel, its connections are closed",
            pending->limits().drain.count()
        );
        pending->scope()->closeTouched();
        endCall(*pending);
        pending->fail(timedOut(pending->limits()));
    }

    void Engine::endCall(Detail::CallState& pending)
    {
        calls_.erase(pending.number);
        pending.timer().cancel();
    }

    RemoteIo::RemoteError Engine::closedError() const
    {
        if (failed())
            return errorOf(RemoteErrorKind::ConnectionLost, "The FTP session failed");
        return errorOf(RemoteErrorKind::ConnectionLost, "The FTP session is closing");
    }

    RemoteIo::RemoteError Engine::wrongThread(std::string_view where)
    {
        return errorOf(RemoteErrorKind::Failure, fmt::format("A synchronous FTP call ran {}", where));
    }

    RemoteIo::RemoteError Engine::timedOut(SyncLimits const& limits)
    {
        return errorOf(
            RemoteErrorKind::TimedOut, fmt::format("The FTP operation took longer than {} ms", limits.backstop.count())
        );
    }

    bool Engine::close(RemoteIo::ProcessingThread& thread, TeardownOptions options)
    {
        assert(
            !strand_.running_in_this_thread() && !onProcessingThread() &&
            "Engine::close() runs on neither the session strand nor the ProcessingThread"
        );
        if (closing_.exchange(true))
            return waitIdle(options.closeGrace);
        Log::debug("FTP: Closing the session");

        // The call running now ends at once, so the task that sends QUIT does not wait behind it.
        post(
            [self = shared_from_this()]()
            {
                self->failCalls(errorOf(RemoteErrorKind::ConnectionLost, "The FTP session was closed"));
            }
        );
        auto quit = thread.pushPromiseTask(
            [self = shared_from_this(), limits = SyncLimits{.backstop = options.quitBackstop, .drain = options.drain}]()
            {
                return self->call<void>(
                    [self](std::shared_ptr<CallScope>) -> asio::awaitable<Result<void>>
                    {
                        if (const auto pool = self->pool())
                            co_await pool->close();
                        co_return Result<void>{};
                    },
                    limits
                );
            }
        );
        if (quit.wait_for(options.quitBackstop + options.drain) != std::future_status::ready)
            Log::warn("FTP: The QUIT of the session did not end in time");

        setStopping();
        post(
            [self = shared_from_this()]()
            {
                self->endSession();
            }
        );
        thread.stop();
        tasks_->close();
        refusePosts();
        return waitIdle(options.closeGrace);
    }

    void Engine::failCalls(RemoteIo::RemoteError const& error)
    {
        const auto calls = std::exchange(calls_, {});
        for (auto const& pending : calls | std::views::values)
        {
            pending->timer().cancel();
            pending->cancel();
            pending->fail(error);
        }
    }

    void Engine::endSession()
    {
        // The owner's handle ends the pool: every connection closes, and no lost handler runs after it.
        pool_.reset();
        failCalls(errorOf(RemoteErrorKind::ConnectionLost, "The FTP session was closed"));
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
        const auto error = errorOf(RemoteErrorKind::ConnectionLost, fmt::format("The FTP session failed: {}", what));
        failCalls(error);
        reportLost(error);
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
