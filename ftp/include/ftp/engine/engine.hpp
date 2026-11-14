#pragma once

#include <ftp/engine/call.hpp>
#include <ftp/engine/resolver.hpp>
#include <ftp/engine/task_gate.hpp>
#include <ftp/engine/work_counter.hpp>
#include <ftp/pool/pool.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace FileTransferProtocol
{
    struct EngineOptions
    {
        PoolOptions pool{};
    };

    /**
     * @brief The bounds of Engine::close().
     */
    struct TeardownOptions
    {
        /**
         * @brief The backstop of the call that sends QUIT on the connections. Longer than PoolOptions::quitTimeout,
         *        which ends a QUIT without an answer, so the backstop only cuts a close of the pool that hangs.
         */
        std::chrono::milliseconds quitBackstop{std::chrono::seconds{2}};
        /** @brief The drain of that call, the closing thread waits for it at most quitBackstop plus this. */
        std::chrono::milliseconds drain{std::chrono::seconds{1}};
        /** @brief How long the close waits for the work of the session at its end. */
        std::chrono::milliseconds closeGrace{std::chrono::seconds{1}};
    };

    /**
     * @brief The engine side of one FTP session: its strand, its pool and the way to its ProcessingThread, on an
     *        executor the engine is handed and never runs, stops or joins.
     *
     * Every handler and coroutine of the session holds the engine, so one that runs late finds a closed engine, never
     * freed memory. Posts and spawns are counted for the teardown, which waits for them with a bound, and refused once
     * refusePosts() ran. An exception that escapes one of them fails the whole session: the pool closes its
     * connections and the lost handler runs. Engine handlers never run backend code, that goes to the ProcessingThread
     * through pushTask(). Operations for callers run bounded through run() and call(), which always answer.
     */
    class Engine : public std::enable_shared_from_this<Engine>
    {
        struct Private
        {};

      public:
        using Strand = boost::asio::strand<boost::asio::any_io_executor>;
        template <typename ValueT>
        using Result = std::expected<ValueT, RemoteIo::RemoteError>;
        /** @brief Runs once, as a task on the ProcessingThread, when the session is lost or failed. */
        using LostHandler = std::function<void(RemoteIo::RemoteError const&)>;

        /**
         * @param executor The application's executor, the engine makes its strands on it.
         * @param tasks The ProcessingThread of the session.
         */
        static std::shared_ptr<Engine> create(
            boost::asio::any_io_executor executor,
            std::shared_ptr<Resolver> resolver,
            std::shared_ptr<TaskGate> tasks,
            EngineOptions options,
            LostHandler onLost = {}
        );

        Engine(Private, boost::asio::any_io_executor executor, std::shared_ptr<TaskGate> tasks, LostHandler onLost);
        ~Engine();
        Engine(Engine const&) = delete;
        Engine& operator=(Engine const&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        Strand const& strand() const;

        /**
         * @brief The pool of the session, used on the session strand. Holding it does not keep it from closing, it is
         *        null once the session failed.
         */
        std::shared_ptr<Pool> pool() const;

        /**
         * @brief Runs @p function on the session strand, from any thread.
         *
         * @return False once posts are refused, @p function is dropped then.
         */
        template <typename FunctionT>
        bool post(FunctionT&& function)
        {
            return postTo(strand_, std::forward<FunctionT>(function));
        }

        /**
         * @brief Runs @p function on @p strand, a strand of this session, from any thread.
         *
         * @return False once posts are refused, @p function is dropped then.
         */
        template <typename FunctionT>
        bool postTo(Strand const& strand, FunctionT&& function)
        {
            std::scoped_lock lock{postMutex_};
            if (postsRefused_)
                return false;
            boost::asio::post(strand, guarded(std::forward<FunctionT>(function)));
            return true;
        }

        /**
         * @brief Runs @p coroutine on the session strand, counted while it runs, from any thread.
         *
         * @return False once posts are refused.
         */
        bool spawn(boost::asio::awaitable<void> coroutine);

        /**
         * @brief Counts what holds it as work of the session, for a handler or coroutine started otherwise.
         */
        WorkCounter::Token work();

        /**
         * @brief Runs the operation @p body returns on the session strand, from any thread, and calls @p done once
         *        with its result. The operation is cancelled at the backstop of @p limits; an error it ends with after
         *        that is TimedOut. When it has not ended after the drain, the connections touched in its CallScope are
         *        closed and it completes as TimedOut, a later result is dropped. @p done runs on the session strand,
         *        or at once on the calling thread while the session is closing, failed or refuses posts. It must not
         *        throw. An operation that learns on its way how much it has to do moves its backstop with
         *        CallScope::allowMore().
         *
         * @param body Called once, on the session strand, with the CallScope of the operation. Returns an
         *        awaitable<Result<ValueT>>, cancellation reaches it as an error, never as an exception.
         */
        template <typename ValueT, typename BodyT>
        void run(BodyT body, SyncLimits limits, std::function<void(Result<ValueT>)> done)
        {
            submitCall<ValueT>(std::move(body), limits, std::move(done), CallRole::Regular);
        }

        /**
         * @brief run() for a task on the ProcessingThread, which waits for the result. Refused on the session strand,
         *        on any thread but the ProcessingThread, and while the session is closing or failed.
         */
        template <typename ValueT, typename BodyT>
        Result<ValueT> call(BodyT body, SyncLimits limits)
        {
            return awaitCall<ValueT>(std::move(body), limits, CallRole::Regular, nullptr);
        }

        /**
         * @brief call() that @p group can end before its time, from any thread: the wait ends at once with the
         *        group's error and the operation is cancelled, then drains as at its backstop.
         */
        template <typename ValueT, typename BodyT>
        Result<ValueT> call(BodyT body, SyncLimits limits, std::shared_ptr<CallGroup> const& group)
        {
            return awaitCall<ValueT>(std::move(body), limits, CallRole::Regular, group);
        }

        /**
         * @brief Queues @p task on the ProcessingThread of the session, from any thread.
         *
         * @return False for an empty task, once the gate is closed or when the thread refused it.
         */
        bool pushTask(std::function<void()> task);

        /**
         * @brief The caller runs on the ProcessingThread of the session.
         */
        bool onProcessingThread() const;

        /**
         * @brief onProcessingThread(), or the task gate is closed, which close() does once the thread stopped, so no
         *        task of the session runs beside the caller: a cleanup task after the close, say.
         */
        bool onProcessingThreadOrClosed() const;

        /**
         * @brief The session ends, synchronous calls are refused from now on. Thread-safe.
         */
        void setStopping();
        bool stopping() const;

        /**
         * @brief An exception escaped a handler or coroutine of the session. Thread-safe.
         */
        bool failed() const;

        /**
         * @brief The pool gave the session up or the session failed: nothing runs on it any more. Thread-safe.
         */
        bool lost() const;

        /**
         * @brief Refuses later posts and spawns, from any thread. Handlers already posted still run.
         */
        void refusePosts();

        /**
         * @brief A handler may still hold the engine after its count ended, so the last reference can drop on an
         *        executor thread after this returned. The engine keeps no raw references and the pool's end goes
         *        through its strand, so that is safe, but the engine is not gone yet.
         *
         * @return Whether every counted handler and coroutine ended within @p limit.
         */
        bool waitIdle(std::chrono::milliseconds limit);

        /**
         * @brief Ends the session, on a thread that is neither an engine thread nor the ProcessingThread, and never
         *        stops the executor: fails the operations running now and refuses later ones, so no call holds up what
         *        follows; sends QUIT on the connections in a final task through call(), the one operation it still
         *        starts; sets stopping, ends the pool and fails what is left; stops the ProcessingThread through the
         *        task gate, its leftover tasks find every call refused, and closes the gate; refuses posts and waits
         *        for the work of the session. A second close waits for the first, then for the work. Once posts are
         *        refused before it, no QUIT is sent, the rest holds.
         *
         * @return Whether the work of the session ended within closeGrace. Work past it owns its state and ends on
         *         an engine thread.
         */
        bool close(TeardownOptions options = {});

      private:
        /**
         * @brief @p function as a handler that holds the engine, counts as work and fails the session when it throws.
         */
        template <typename FunctionT>
        auto guarded(FunctionT&& function)
        {
            return [self = shared_from_this(),
                       token = work(),
                       function = std::forward<FunctionT>(function)](auto&&... arguments) mutable
            {
                try
                {
                    function(std::forward<decltype(arguments)>(arguments)...);
                }
                catch (...)
                {
                    self->handlerThrew(std::current_exception());
                }
            };
        }

        /**
         * @brief Runs @p function on the session strand also once posts are refused, for what ends the session.
         */
        template <typename FunctionT>
        void postAlways(FunctionT&& function)
        {
            boost::asio::post(strand_, guarded(std::forward<FunctionT>(function)));
        }

        /**
         * @brief Which operations a session that closes still starts: only the QUIT of close().
         */
        enum class CallRole
        {
            Regular,
            Quit,
        };

        /**
         * @return The operation, which holds itself while it runs.
         */
        template <typename ValueT, typename BodyT>
        std::weak_ptr<Detail::CallState>
        submitCall(BodyT body, SyncLimits limits, std::function<void(Result<ValueT>)> done, CallRole role)
        {
            const auto pending = std::make_shared<Detail::Call<ValueT>>(strand_, limits, std::move(done));
            if (refusesCalls(role))
            {
                pending->fail(closedError());
                return {};
            }
            const auto posted = post(
                [self = shared_from_this(), pending, body = std::move(body), role]() mutable
                {
                    self->startCall(pending, std::move(body), role);
                }
            );
            if (!posted)
                pending->fail(closedError());
            return pending;
        }

        template <typename ValueT, typename BodyT>
        Result<ValueT>
        awaitCall(BodyT body, SyncLimits limits, CallRole role, std::shared_ptr<CallGroup> const& group)
        {
            if (strand_.running_in_this_thread())
                return std::unexpected(wrongThread("on the session strand"));
            if (!onProcessingThreadOrClosed())
                return std::unexpected(wrongThread("outside the processing thread"));
            if (auto ended = group ? group->endedWith() : std::nullopt)
                return std::unexpected(std::move(*ended));
            const auto outcome = std::make_shared<Detail::SyncOutcome<ValueT>>();
            auto future = outcome->future();
            const auto pending = submitCall<ValueT>(
                std::move(body),
                limits,
                [outcome](Result<ValueT> result)
                {
                    outcome->set(std::move(result));
                },
                role
            );
            if (!group)
                return future.get();
            const auto ender = [outcome, weakSelf = weak_from_this(), pending](RemoteIo::RemoteError const& error)
            {
                outcome->set(std::unexpected(error));
                if (const auto self = weakSelf.lock())
                    self->cutShort(pending);
            };
            const auto member = group->join(ender);
            if (!member)
                ender(group->endedWith().value_or(closedError()));
            auto result = future.get();
            if (member)
                group->leave(*member);
            return result;
        }

        template <typename ValueT, typename BodyT>
        void startCall(std::shared_ptr<Detail::Call<ValueT>> const& pending, BodyT body, CallRole role)
        {
            if (!beginCall(pending, role))
                return;
            boost::asio::co_spawn(
                strand_,
                [body = std::move(body), scope = pending->scope()]() mutable -> boost::asio::awaitable<Result<ValueT>>
                {
                    co_await boost::asio::this_coro::throw_if_cancelled(false);
                    co_return co_await body(scope);
                },
                boost::asio::bind_cancellation_slot(
                    pending->slot(),
                    guarded(
                        [self = shared_from_this(), pending](std::exception_ptr exception, Result<ValueT> result)
                        {
                            // Failing the session completes the operation.
                            if (exception)
                                std::rethrow_exception(exception);
                            self->endCall(*pending);
                            if (!result && pending->draining())
                                pending->fail(timedOut(pending->limits()));
                            else
                                pending->complete(std::move(result));
                        }
                    )
                )
            );
        }

        /**
         * @brief Registers @p pending and starts its backstop, on the session strand.
         *
         * @return False when the session refuses @p role, @p pending is completed then.
         */
        bool beginCall(std::shared_ptr<Detail::CallState> const& pending, CallRole role);
        /**
         * @brief The session is stopping or failed, or closing and @p role is not its QUIT. Thread-safe.
         */
        bool refusesCalls(CallRole role) const;
        void armCallTimer(std::shared_ptr<Detail::CallState> const& pending, std::chrono::milliseconds duration);
        void awaitCallTimer(std::shared_ptr<Detail::CallState> const& pending);
        /**
         * @brief Moves the backstop of @p pending later by @p more unless it was cancelled, on the session strand.
         */
        void extendCall(std::shared_ptr<Detail::CallState> const& pending, std::chrono::milliseconds more);
        void callTimeUp(std::shared_ptr<Detail::CallState> const& pending);
        /**
         * @brief Cancels @p pending and gives it the drain to end, on the session strand.
         */
        void beginDrain(std::shared_ptr<Detail::CallState> const& pending);
        /**
         * @brief Cancels @p pending as at its backstop, from any thread. Nothing once it ended or drains.
         */
        void cutShort(std::weak_ptr<Detail::CallState> pending);
        void endCall(Detail::CallState& pending);
        RemoteIo::RemoteError closedError() const;
        static RemoteIo::RemoteError wrongThread(std::string_view where);
        static RemoteIo::RemoteError timedOut(SyncLimits const& limits);

        /**
         * @brief Completes the operations in flight with @p error and cancels them, on the session strand.
         */
        void failCalls(RemoteIo::RemoteError const& error);
        /**
         * @brief Ends the pool and fails the operations in flight, on the session strand.
         */
        void endSession();
        void handlerThrew(std::exception_ptr exception);
        void fail(std::exception_ptr exception);
        void reportLost(RemoteIo::RemoteError error);

        const Strand strand_;
        const std::shared_ptr<TaskGate> tasks_;
        const LostHandler onLost_;
        const std::shared_ptr<WorkCounter> work_;
        /** @brief The owner's handle of the pool, the session strand's alone. */
        std::shared_ptr<Pool> pool_{};
        std::mutex postMutex_{};
        bool postsRefused_{false};
        /** @brief The operations of run() in flight, the session strand's alone. */
        std::unordered_map<std::uint64_t, std::shared_ptr<Detail::CallState>> calls_{};
        std::uint64_t callNumber_{0u};
        std::atomic<bool> stopping_{false};
        std::atomic<bool> failed_{false};
        std::atomic<bool> lost_{false};
        /** @brief Held through close(), so a second close returns only after the first. */
        std::mutex closeMutex_{};
        std::atomic<bool> closing_{false};
    };
}
