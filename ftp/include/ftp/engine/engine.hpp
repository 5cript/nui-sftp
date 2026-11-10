#pragma once

#include <ftp/engine/resolver.hpp>
#include <ftp/engine/task_gate.hpp>
#include <ftp/engine/work_counter.hpp>
#include <ftp/pool/pool.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace FileTransferProtocol
{
    struct EngineOptions
    {
        PoolOptions pool{};
    };

    /**
     * @brief The engine side of one FTP session: its strand, its pool and the way to its ProcessingThread, on an
     *        executor the engine is handed and never runs, stops or joins.
     *
     * Every handler and coroutine of the session holds the engine, so one that runs late finds a closed engine, never
     * freed memory. Posts and spawns are counted for the teardown, which waits for them with a bound, and refused once
     * refusePosts() ran. An exception that escapes one of them fails the whole session: the pool closes its
     * connections and the lost handler runs. Engine handlers never run backend code, that goes to the ProcessingThread
     * through pushTask().
     */
    class Engine : public std::enable_shared_from_this<Engine>
    {
        struct Private
        {};

      public:
        using Strand = boost::asio::strand<boost::asio::any_io_executor>;
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
            boost::asio::post(
                strand,
                [self = shared_from_this(), token = work(), function = std::forward<FunctionT>(function)]() mutable
                {
                    try
                    {
                        function();
                    }
                    catch (...)
                    {
                        self->handlerThrew(std::current_exception());
                    }
                }
            );
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
         * @brief The session ends, synchronous calls are refused from now on. Thread-safe.
         */
        void setStopping();
        bool stopping() const;

        /**
         * @brief An exception escaped a handler or coroutine of the session. Thread-safe.
         */
        bool failed() const;

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

      private:
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
        std::atomic<bool> stopping_{false};
        std::atomic<bool> failed_{false};
        std::atomic<bool> lostReported_{false};
    };
}
