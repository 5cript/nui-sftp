#include <ftp/engine/supply_ring.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/this_coro.hpp>

#include <cassert>
#include <exception>
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

    std::shared_ptr<SupplyRing>
    SupplyRing::create(std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options)
    {
        return std::make_shared<SupplyRing>(Private{}, std::move(engine), std::move(connectionStrand), options);
    }

    SupplyRing::SupplyRing(Private, std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options)
        : engine_{std::move(engine)}
        , strand_{std::move(connectionStrand)}
        , slots_(options.slots, std::vector<char>(options.chunkSize))
        , wakeTimer_{strand_, asio::steady_timer::time_point::max()}
    {
        assert(options.slots > 0u && options.chunkSize > 0u && "A supply ring needs a slot of at least one byte");
        for (std::size_t slot = 0u; slot < slots_.size(); ++slot)
            free_.push_back(slot);
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> SupplyRing::run(Source source, Sink sink, Paused paused)
    {
        return runHolding(shared_from_this(), std::move(source), std::move(sink), std::move(paused));
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    SupplyRing::runHolding(std::shared_ptr<SupplyRing> self, Source source, Sink sink, Paused paused)
    {
        assert(
            self->strand_.running_in_this_thread() && !self->running_ && "SupplyRing::run() runs once, on its strand"
        );
        self->running_ = true;
        self->source_ = std::move(source);
        self->paused_ = std::move(paused);
        std::expected<void, RemoteIo::RemoteError> outcome{};
        std::exception_ptr thrown{};
        try
        {
            outcome = co_await self->writeSlots(std::move(sink));
        }
        catch (...)
        {
            thrown = std::current_exception();
        }
        // The source and the pause may hold what holds the ring, also when the sink threw.
        self->finished_ = true;
        self->paused_ = nullptr;
        self->letGoOfTheSourceWhenIdle();
        if (thrown)
            std::rethrow_exception(thrown);
        co_return outcome;
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> SupplyRing::writeSlots(Sink sink)
    {
        auto ended = false;
        while (!ended)
        {
            // A cancelled coroutine starts no operation, an awaited one would throw.
            if (isCancelled(co_await asio::this_coro::cancellation_state))
                stopped_ = true;
            if (stopped_)
                co_return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The upload was stopped"));
            if (failure_)
                co_return std::unexpected(*failure_);
            if (!handOut())
            {
                co_return std::unexpected(
                    errorOf(RemoteErrorKind::ConnectionLost, "The session's thread takes no more fills")
                );
            }
            if (filled_.empty() || isPaused())
            {
                co_await wakeTimer_.async_wait(useTuple);
                continue;
            }

            const auto next = filled_.front();
            filled_.pop_front();
            std::expected<void, RemoteIo::RemoteError> written{};
            if (next.count > 0u)
                written = co_await sink(asio::buffer(slots_[next.slot].data(), next.count));
            free_.push_back(next.slot);
            if (!written)
                co_return std::unexpected(written.error());
            ended = next.ended;
        }
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    void SupplyRing::wake()
    {
        engine_->postTo(
            strand_,
            [self = shared_from_this()]()
            {
                self->wakeTimer_.cancel();
            }
        );
    }

    void SupplyRing::stop()
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

    void SupplyRing::close()
    {
        assert(engine_->onProcessingThreadOrClosed() && "SupplyRing::close() runs on the ProcessingThread");
        if (closed_)
            return;
        closed_ = true;
        stop();
    }

    bool SupplyRing::handOut()
    {
        while (!endSeen_ && !isPaused() && !free_.empty())
        {
            const auto slot = free_.front();
            free_.pop_front();
            ++fillsOut_;
            const auto pushed = engine_->pushTask(
                [self = shared_from_this(), slot]()
                {
                    self->fill(slot);
                }
            );
            if (!pushed)
            {
                --fillsOut_;
                free_.push_back(slot);
                return false;
            }
        }
        return true;
    }

    void SupplyRing::fill(std::size_t slot)
    {
        auto& bytes = slots_[slot];
        std::size_t count{0u};
        std::optional<RemoteIo::RemoteError> failure{};
        while (!closed_ && !sourceEnded_ && !sourceFailed_ && count < bytes.size())
        {
            const auto got = source_(std::span<char>{bytes.data() + count, bytes.size() - count});
            if (!got)
            {
                sourceFailed_ = true;
                failure = got.error();
                continue;
            }
            assert(*got <= bytes.size() - count && "A source put more bytes into the span than it holds");
            sourceEnded_ = *got == 0u;
            count += *got;
        }
        std::expected<Filled, RemoteIo::RemoteError> filled{
            Filled{.slot = slot, .count = count, .ended = sourceEnded_}
        };
        if (failure)
            filled = std::unexpected(std::move(*failure));
        engine_->postTo(
            strand_,
            [self = shared_from_this(), slot, filled = std::move(filled)]() mutable
            {
                self->takeFilled(slot, std::move(filled));
            }
        );
    }

    void SupplyRing::takeFilled(std::size_t slot, std::expected<Filled, RemoteIo::RemoteError> filled)
    {
        --fillsOut_;
        if (filled)
        {
            endSeen_ = endSeen_ || filled->ended;
            filled_.push_back(*filled);
        }
        else
        {
            free_.push_back(slot);
            if (!failure_)
                failure_ = std::move(filled.error());
        }
        wakeTimer_.cancel();
        letGoOfTheSourceWhenIdle();
    }

    bool SupplyRing::isPaused() const
    {
        return paused_ && paused_();
    }

    void SupplyRing::letGoOfTheSourceWhenIdle()
    {
        if (finished_ && fillsOut_ == 0u)
            source_ = nullptr;
    }
}
