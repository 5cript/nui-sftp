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
    }

    std::shared_ptr<DeliveryRing>
    DeliveryRing::create(std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options)
    {
        return std::make_shared<DeliveryRing>(Private{}, std::move(engine), std::move(connectionStrand), options);
    }

    DeliveryRing::DeliveryRing(Private, std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options)
        : engine_{std::move(engine)}
        , strand_{std::move(connectionStrand)}
        , slots_(options.slots, std::vector<char>(options.chunkSize))
        , wakeTimer_{strand_, asio::steady_timer::time_point::max()}
    {
        assert(options.slots > 0u && options.chunkSize > 0u && "A delivery ring needs a slot of at least one byte");
        for (std::size_t slot = 0u; slot < slots_.size(); ++slot)
            free_.push_back(slot);
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    DeliveryRing::run(Source source, Sink sink, Paused paused)
    {
        return runHolding(shared_from_this(), std::move(source), std::move(sink), std::move(paused));
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    DeliveryRing::runHolding(std::shared_ptr<DeliveryRing> self, Source source, Sink sink, Paused paused)
    {
        assert(
            self->strand_.running_in_this_thread() && !self->running_ && "DeliveryRing::run() runs once, on its strand"
        );
        self->running_ = true;
        self->sink_ = std::move(sink);
        self->paused_ = std::move(paused);
        auto outcome = co_await self->fillSlots(std::move(source));
        // The sink and the pause may hold what holds the ring.
        self->finished_ = true;
        self->paused_ = nullptr;
        self->letGoOfTheSinkWhenIdle();
        co_return outcome;
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> DeliveryRing::fillSlots(Source source)
    {
        const auto self = shared_from_this();
        const auto stopped = []()
        {
            return errorOf(RemoteErrorKind::Cancelled, "The download was stopped");
        };
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
                co_return std::unexpected(stopped());

            const auto slot = free_.front();
            free_.pop_front();
            auto& bytes = slots_[slot];
            std::size_t filled{0u};
            while (!ended && filled < bytes.size())
            {
                const auto received = co_await source(asio::buffer(bytes.data() + filled, bytes.size() - filled));
                if (!received)
                {
                    free_.push_back(slot);
                    co_return std::unexpected(received.error());
                }
                ended = *received == 0u;
                filled += *received;
                if (isCancelled(co_await asio::this_coro::cancellation_state))
                    stopped_ = true;
                if (stopped_)
                {
                    free_.push_back(slot);
                    co_return std::unexpected(stopped());
                }
            }
            if (filled == 0u)
            {
                free_.push_back(slot);
                continue;
            }
            const auto pushed = engine_->pushTask(
                [self, slot, filled]()
                {
                    self->deliver(slot, filled);
                }
            );
            if (!pushed)
            {
                free_.push_back(slot);
                co_return std::unexpected(
                    errorOf(RemoteErrorKind::ConnectionLost, "The session's thread takes no more deliveries")
                );
            }
        }
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    void DeliveryRing::wake()
    {
        engine_->postTo(
            strand_,
            [self = shared_from_this()]()
            {
                self->wakeTimer_.cancel();
            }
        );
    }

    void DeliveryRing::stop()
    {
        engine_->postTo(
            strand_,
            [self = shared_from_this()]()
            {
                self->stopped_ = true;
                self->wakeTimer_.cancel();
            }
        );
    }

    void DeliveryRing::close()
    {
        assert(engine_->onProcessingThreadOrClosed() && "DeliveryRing::close() runs on the ProcessingThread");
        if (closed_)
            return;
        closed_ = true;
        stop();
    }

    void DeliveryRing::deliver(std::size_t slot, std::size_t filled)
    {
        if (!closed_ && !sink_(std::span<char const>{slots_[slot].data(), filled}))
            close();
        giveBack(slot);
    }

    void DeliveryRing::giveBack(std::size_t slot)
    {
        engine_->postTo(
            strand_,
            [self = shared_from_this(), slot]()
            {
                self->free_.push_back(slot);
                self->wakeTimer_.cancel();
                self->letGoOfTheSinkWhenIdle();
            }
        );
    }

    void DeliveryRing::letGoOfTheSinkWhenIdle()
    {
        if (finished_ && free_.size() == slots_.size())
            sink_ = nullptr;
    }

    bool DeliveryRing::mayRead() const
    {
        return !free_.empty() && !(paused_ && paused_());
    }
}
