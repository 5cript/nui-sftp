#include <ftp/engine/delivery_ring.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/this_coro.hpp>

#include <cassert>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Detail::errorOf;
        using Detail::isCancelled;
        using Detail::useTuple;
        using RemoteIo::RemoteErrorKind;

        RemoteIo::RemoteError lostDeliveries()
        {
            return errorOf(RemoteErrorKind::ConnectionLost, "The session's thread takes no more deliveries");
        }

        /**
         * @brief A free slot taken to be filled. It goes back to the free ones when the fill ends, also on an
         *        exception of the source, unless a delivery took it over.
         */
        class SlotLoan
        {
          public:
            explicit SlotLoan(std::deque<std::size_t>& free)
                : free_{&free}
                , slot_{free.front()}
            {
                free.pop_front();
            }
            ~SlotLoan()
            {
                if (free_ != nullptr)
                    free_->push_back(slot_);
            }
            SlotLoan(SlotLoan const&) = delete;
            SlotLoan& operator=(SlotLoan const&) = delete;
            SlotLoan(SlotLoan&&) = delete;
            SlotLoan& operator=(SlotLoan&&) = delete;

            std::size_t slot() const
            {
                return slot_;
            }

            /**
             * @brief The delivery of the slot gives it back from now on.
             */
            void handOver() noexcept
            {
                free_ = nullptr;
            }

          private:
            std::deque<std::size_t>* free_;
            std::size_t slot_;
        };
    }

    /**
     * @brief One delivery, owned by its task. Gives the slot back when the task ends, as lost unless the delivery
     *        ended: when the ProcessingThread dropped the task unrun, a refused push among them.
     */
    class DeliveryRing::DeliveryTask
    {
      public:
        DeliveryTask(std::shared_ptr<DeliveryRing> ring, std::size_t slot, std::size_t filled)
            : ring_{std::move(ring)}
            , slot_{slot}
            , filled_{filled}
        {}
        ~DeliveryTask()
        {
            ring_->giveBack(slot_, delivered_);
        }
        DeliveryTask(DeliveryTask const&) = delete;
        DeliveryTask& operator=(DeliveryTask const&) = delete;
        DeliveryTask(DeliveryTask&&) = delete;
        DeliveryTask& operator=(DeliveryTask&&) = delete;

        void run()
        {
            ring_->deliver(slot_, filled_);
            delivered_ = true;
        }

      private:
        const std::shared_ptr<DeliveryRing> ring_;
        const std::size_t slot_;
        const std::size_t filled_;
        bool delivered_{false};
    };

    std::shared_ptr<DeliveryRing>
    DeliveryRing::create(std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options)
    {
        return std::make_shared<DeliveryRing>(Private{}, std::move(engine), std::move(connectionStrand), options);
    }

    DeliveryRing::DeliveryRing(Private, std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options)
        : Ring{std::move(engine), std::move(connectionStrand), options}
    {}

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    DeliveryRing::run(Source source, Sink sink, Paused paused)
    {
        return runHolding(shared_from_this(), std::move(paused), fillSlots(std::move(source), std::move(sink)));
    }

    asio::awaitable<Ring::Outcome> DeliveryRing::fillSlots(Source source, Sink sink)
    {
        const auto self = sharedAs<DeliveryRing>();
        sink_ = std::move(sink);
        // A cancelled coroutine starts no operation, an awaited one would throw.
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            stopped_ = true;
        auto ended = false;
        while (!ended)
        {
            while (!stopped_ && !mayRead())
            {
                co_await wakeTimer_.async_wait(useTuple);
                if (isCancelled(co_await asio::this_coro::cancellation_state))
                    stopped_ = true;
            }
            if (stopped_)
                co_return std::unexpected(stopReason());

            SlotLoan loan{free_};
            const auto bytes = bytesOf(loan.slot());
            std::size_t filled{0u};
            while (!ended && filled < bytes.size())
            {
                const auto received = co_await source(asio::buffer(bytes.data() + filled, bytes.size() - filled));
                if (!received)
                    co_return std::unexpected(received.error());
                assert(*received <= bytes.size() - filled && "A source put more bytes into the buffer than it holds");
                ended = *received == 0u;
                filled += *received;
                if (isCancelled(co_await asio::this_coro::cancellation_state))
                    stopped_ = true;
                if (stopped_)
                    co_return std::unexpected(stopReason());
            }
            if (filled == 0u)
                continue;
            // Made before the loan ends: a delivery that cannot be made leaves the slot with the loan.
            auto delivery = std::make_shared<DeliveryTask>(self, loan.slot(), filled);
            loan.handOver();
            const auto pushed = engine_->pushTask(
                [delivery = std::move(delivery)]() mutable
                {
                    // Ends with the delivery, the thread keeps a task until its batch ran.
                    const auto ending = std::move(delivery);
                    ending->run();
                }
            );
            // The refused task gave the slot back.
            if (!pushed)
                co_return std::unexpected(lostDeliveries());
        }
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    void DeliveryRing::deliver(std::size_t slot, std::size_t filled)
    {
        if (closed_)
            return;
        if (!sink_(bytesOf(slot).first(filled)))
            close();
    }

    void DeliveryRing::giveBack(std::size_t slot, bool delivered)
    {
        auto takeBack = [self = sharedAs<DeliveryRing>(), slot, delivered]()
        {
            self->free_.push_back(slot);
            if (!delivered)
            {
                self->lost_ = true;
                self->stopped_ = true;
            }
            self->wakeTimer_.cancel();
            self->letGoWhenIdle();
        };
        // A refused push ends its task on the strand, where the engine may refuse posts already.
        if (strand_.running_in_this_thread())
            takeBack();
        else
            engine_->postTo(strand_, std::move(takeBack));
    }

    RemoteIo::RemoteError DeliveryRing::stopReason() const
    {
        if (lost_)
            return lostDeliveries();
        return errorOf(RemoteErrorKind::Cancelled, "The download was stopped");
    }

    void DeliveryRing::letGoWhenIdle()
    {
        if (finished_ && free_.size() == slotCount())
            sink_ = nullptr;
    }

    bool DeliveryRing::mayRead() const
    {
        return !free_.empty() && !isPaused();
    }
}
