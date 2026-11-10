#pragma once

#include <ftp/engine/engine.hpp>
#include <ftp/engine/ring_options.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <vector>

namespace FileTransferProtocol
{
    /**
     * @brief What the rings of an asynchronous transfer share: the slots that go between the connection strand and the
     *        ProcessingThread, the wait on the strand while no slot is free or the transfer is paused, and the end of a
     *        run, which lets go of what may hold the ring.
     */
    class Ring : public std::enable_shared_from_this<Ring>
    {
      public:
        using Strand = Engine::Strand;
        /** @brief Whether the transfer is paused, from the connection strand. */
        using Paused = std::function<bool()>;

        virtual ~Ring() = default;
        Ring(Ring const&) = delete;
        Ring& operator=(Ring const&) = delete;
        Ring(Ring&&) = delete;
        Ring& operator=(Ring&&) = delete;

        /**
         * @brief Wakes a ring that waits while paused. Any thread.
         */
        void wake();

        /**
         * @brief Ends run() at its next turn, an operation of the connection under way ends first. Any thread.
         */
        void stop();

        /**
         * @brief Stops the ring, and the ProcessingThread side of the transfer, the sink of a download or the source of
         *        an upload, is not called after this returned. On the ProcessingThread, also inside that side, or once
         *        the session closed.
         */
        void close();

      protected:
        using Outcome = std::expected<void, RemoteIo::RemoteError>;

        Ring(std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options);

        /**
         * @brief Runs @p transfer once, on the connection strand, with @p self in the coroutine frame, which may start
         *        after its caller let go of the ring. Afterwards, also when @p transfer threw, the pause is dropped and
         *        letGoWhenIdle() runs.
         */
        static boost::asio::awaitable<Outcome>
        runHolding(std::shared_ptr<Ring> self, Paused paused, boost::asio::awaitable<Outcome> transfer);

        /**
         * @brief Drops the ProcessingThread side once the run ended and no task of the ring is out, on the connection
         *        strand.
         */
        virtual void letGoWhenIdle() = 0;

        bool isPaused() const;

        /**
         * @brief shared_from_this() as the ring @p RingT that derives from this one.
         */
        template <typename RingT>
        std::shared_ptr<RingT> sharedAs()
        {
            return std::static_pointer_cast<RingT>(shared_from_this());
        }

        const std::shared_ptr<Engine> engine_;
        const Strand strand_;
        std::vector<std::vector<char>> slots_;
        /** @brief The connection strand's. */
        std::deque<std::size_t> free_{};
        boost::asio::steady_timer wakeTimer_;
        bool stopped_{false};
        /** @brief run() ended. */
        bool finished_{false};
        /** @brief The ProcessingThread's. */
        bool closed_{false};

      private:
        bool running_{false};
        Paused paused_{};
    };
}
