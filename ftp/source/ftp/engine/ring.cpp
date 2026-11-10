#include <ftp/engine/ring.hpp>

#include <cassert>
#include <exception>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
    }

    Ring::Ring(std::shared_ptr<Engine> engine, Strand connectionStrand, RingOptions options)
        : engine_{std::move(engine)}
        , strand_{std::move(connectionStrand)}
        , wakeTimer_{strand_, asio::steady_timer::time_point::max()}
        , chunkSize_{options.chunkSize}
    {
        assert(options.slots > 0u && options.chunkSize > 0u && "A ring needs a slot of at least one byte");
        // Not zeroed: every byte is written before it is read, and a small transfer touches few pages.
        for (std::size_t slot = 0u; slot < options.slots; ++slot)
        {
            slots_.push_back(std::make_unique_for_overwrite<char[]>(chunkSize_));
            free_.push_back(slot);
        }
    }

    void Ring::wake()
    {
        engine_->postTo(
            strand_,
            [self = shared_from_this()]()
            {
                self->wakeTimer_.cancel();
            }
        );
    }

    void Ring::stop()
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

    void Ring::close()
    {
        assert(engine_->onProcessingThreadOrClosed() && "Ring::close() runs on the ProcessingThread");
        if (closed_)
            return;
        closed_ = true;
        stop();
    }

    asio::awaitable<Ring::Outcome>
    Ring::runHolding(std::shared_ptr<Ring> self, Paused paused, asio::awaitable<Outcome> transfer)
    {
        assert(self->strand_.running_in_this_thread() && !self->running_ && "A ring runs once, on its strand");
        self->running_ = true;
        self->paused_ = std::move(paused);
        Outcome outcome{};
        std::exception_ptr thrown{};
        try
        {
            outcome = co_await std::move(transfer);
        }
        catch (...)
        {
            thrown = std::current_exception();
        }
        // The pause and the ProcessingThread side may hold what holds the ring, also when the transfer threw.
        self->finished_ = true;
        self->paused_ = nullptr;
        self->letGoWhenIdle();
        if (thrown)
            std::rethrow_exception(thrown);
        co_return outcome;
    }

    bool Ring::isPaused() const
    {
        return paused_ && paused_();
    }

    std::span<char> Ring::bytesOf(std::size_t slot)
    {
        return {slots_[slot].get(), chunkSize_};
    }

    std::size_t Ring::slotCount() const
    {
        return slots_.size();
    }
}
