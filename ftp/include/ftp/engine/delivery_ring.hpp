#pragma once

#include <ftp/engine/engine.hpp>
#include <ftp/engine/ring.hpp>
#include <ftp/engine/ring_options.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>

namespace FileTransferProtocol
{
    /**
     * @brief Carries one asynchronous download from its connection strand to the ProcessingThread. The strand reads
     *        into a free slot until it is full and hands it over as a task, the task gives it back with a post. Without
     *        a free slot, or while paused, no read is outstanding, so the inactivity deadlines of the reads do not run.
     *
     * Deliveries keep the ring alive, and the end of run() is pushed after the last of them, so a completion task the
     * caller pushes then runs after every delivery. The ring lets go of the sink once it ended and every slot is back,
     * so the sink may hold what holds the ring. Stop the ring before the engine refuses posts. close() ends the sink:
     * later deliveries drop their bytes and give the slot back.
     */
    class DeliveryRing : public Ring
    {
        struct Private
        {};

      public:
        /** @brief Reads the next bytes into the buffer, 0 once the data ended. Called on the connection strand. */
        using Source = std::function<
            boost::asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>(boost::asio::mutable_buffer)>;
        /** @brief Takes the bytes of one slot on the ProcessingThread, false refuses them and stops the ring. */
        using Sink = std::function<bool(std::span<char const>)>;

        static std::shared_ptr<DeliveryRing>
        create(std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options = {});

        DeliveryRing(Private, std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options);
        ~DeliveryRing() override = default;
        DeliveryRing(DeliveryRing const&) = delete;
        DeliveryRing& operator=(DeliveryRing const&) = delete;
        DeliveryRing(DeliveryRing&&) = delete;
        DeliveryRing& operator=(DeliveryRing&&) = delete;

        /**
         * @brief Reads with @p source and hands every filled slot to @p sink until the data ends. Runs once, on the
         *        connection strand; a cancel of the coroutine stops it like stop().
         *
         * @param paused Asked before each slot, the ring waits while it is true. Call wake() when it turns false.
         * @return Nothing once the data ended and every filled slot was handed over; the error of a failed read;
         *         Cancelled after stop(), close() or a refused delivery; ConnectionLost when the ProcessingThread takes
         *         no more tasks. A slot that was not full yet is dropped with the error.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
        run(Source source, Sink sink, Paused paused = {});

      private:
        boost::asio::awaitable<Outcome> fillSlots(Source source, Sink sink);
        void deliver(std::size_t slot, std::size_t filled);
        void giveBack(std::size_t slot);
        bool mayRead() const;
        /**
         * @brief Drops the sink once run() ended and every delivery gave its slot back.
         */
        void letGoWhenIdle() override;

        /**
         * @brief Used by the deliveries. Set on the connection strand before the first is pushed, dropped there after
         *        the last gave its slot back.
         */
        Sink sink_{};
    };
}
