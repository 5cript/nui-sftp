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
                std::expected<std::size_t, RemoteIo::RemoteError> received{};
                try
                {
                    received = co_await source(asio::buffer(bytes.data() + filled, bytes.size() - filled));
                }
                catch (...)
                {
                    // Every slot must come back before the ring lets go of the sink.
                    free_.push_back(slot);
                    throw;
                }
                if (!received)
                {
                    free_.push_back(slot);
                    co_return std::unexpected(received.error());
                }
                assert(*received <= bytes.size() - filled && "A source put more bytes into the buffer than it holds");
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
            [self = sharedAs<DeliveryRing>(), slot]()
            {
                self->free_.push_back(slot);
                self->wakeTimer_.cancel();
                self->letGoWhenIdle();
            }
        );
    }

    void DeliveryRing::letGoWhenIdle()
    {
        if (finished_ && free_.size() == slots_.size())
            sink_ = nullptr;
    }

    bool DeliveryRing::mayRead() const
    {
        return !free_.empty() && !isPaused();
    }
}
