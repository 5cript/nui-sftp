#pragma once

#include <ftp/engine/engine.hpp>
#include <ftp/engine/ring.hpp>
#include <ftp/engine/ring_options.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>

#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace FileTransferProtocol
{
    /**
     * @brief Carries one asynchronous upload from the ProcessingThread to its connection strand, the mirror of
     *        DeliveryRing. A task fills a free slot from the source until it is full and posts it to the strand, which
     *        writes the slots in order and hands each one out to be filled again. Without a free slot, or while
     *        paused, no fill is outstanding.
     *
     * Fills keep the ring alive. The ring lets go of the source once it ended and every fill came back, so the source
     * may hold what holds the ring. Stop the ring before the engine refuses posts. close() ends the source, a write
     * under way ends before run() does. A fill comes back also when its source throws or the ProcessingThread drops it
     * unrun, which ends the ring as lost, and the source is not called again.
     */
    class SupplyRing : public Ring
    {
        struct Private
        {};

      public:
        /** @brief Puts the next bytes into the span, 0 once the data ended. Called on the ProcessingThread. */
        using Source = std::function<std::expected<std::size_t, RemoteIo::RemoteError>(std::span<char>)>;
        /** @brief Writes all of the buffer, on the connection strand. */
        using Sink = std::function<
            boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>>(boost::asio::const_buffer)>;

        static std::shared_ptr<SupplyRing>
        create(std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options = {});

        SupplyRing(Private, std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options);
        ~SupplyRing() override = default;
        SupplyRing(SupplyRing const&) = delete;
        SupplyRing& operator=(SupplyRing const&) = delete;
        SupplyRing(SupplyRing&&) = delete;
        SupplyRing& operator=(SupplyRing&&) = delete;

        /**
         * @brief Fills slots from @p source and writes them with @p sink until the data ended and the last slot is
         *        written. Runs once, on the connection strand; a cancel of the coroutine stops it like stop().
         *
         * @param paused Asked before each fill and write, the ring waits while it is true. Call wake() when it turns
         *        false.
         * @return Nothing once every byte of the source was written; the error of a failed fill or write; Cancelled
         *         after stop() or close(); ConnectionLost when the ProcessingThread takes no more tasks, dropped a
         *         fill, or the source threw.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
        run(Source source, Sink sink, Paused paused = {});

      private:
        /**
         * @brief A slot back from the ProcessingThread.
         */
        struct Filled
        {
            std::size_t slot;
            std::size_t count;
            /** @brief The source ended within this slot, it is the last one to write. */
            bool ended;
        };

        class FillTask;
        using FillOutcome = std::expected<Filled, RemoteIo::RemoteError>;

        boost::asio::awaitable<Outcome> writeSlots(Source source, Sink sink);
        /**
         * @brief Pushes a fill for every free slot while the data goes on and the ring is not paused.
         *
         * @return False when the ProcessingThread refused one.
         */
        bool handOut();
        /**
         * @brief Fills @p slot from the source, on the ProcessingThread.
         */
        FillOutcome fill(std::size_t slot);
        /**
         * @brief Hands @p slot with what its fill came to back to the connection strand, from any thread.
         */
        void handBack(std::size_t slot, FillOutcome filled);
        void takeFilled(std::size_t slot, FillOutcome filled);
        /**
         * @brief Drops the source once run() ended and every fill came back.
         */
        void letGoWhenIdle() override;

        /** @brief Filled slots in the order of the data, the connection strand's. */
        std::deque<Filled> filled_{};
        std::size_t fillsOut_{0u};
        /** @brief A fill came back with the end of the data. */
        bool endSeen_{false};
        std::optional<RemoteIo::RemoteError> failure_{};
        /**
         * @brief Used by the fills. Set on the connection strand before the first is pushed, dropped there after the
         *        last came back.
         */
        Source source_{};
        /** @brief The source returned the end, later fills do not call it. The ProcessingThread's. */
        bool sourceEnded_{false};
        /** @brief The source returned an error or threw, later fills do not call it. The ProcessingThread's. */
        bool sourceFailed_{false};
    };
}
