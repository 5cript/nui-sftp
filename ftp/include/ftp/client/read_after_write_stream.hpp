#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace FileTransferProtocol::Detail
{
    /**
     * @brief The stream under an Asio ssl::stream, it hands a read that completed while a write was in flight to the
     *        TLS stream once that write is through. Output OpenSSL makes inside a read that had to wait for a write,
     *        such as the alert that refuses a renegotiation, stays in the TLS stream's buffer until a later write adds
     *        more, so without a later write it never leaves. Used from one strand only.
     */
    template <typename LowerT>
    class ReadAfterWriteStream
    {
      public:
        using executor_type = typename LowerT::executor_type;
        using lowest_layer_type = typename LowerT::lowest_layer_type;

        explicit ReadAfterWriteStream(LowerT lower)
            : lower_{std::move(lower)}
        {}

        executor_type get_executor()
        {
            return lower_.get_executor();
        }

        lowest_layer_type& lowest_layer()
        {
            return lower_.lowest_layer();
        }

        lowest_layer_type const& lowest_layer() const
        {
            return lower_.lowest_layer();
        }

        template <typename BuffersT, typename HandlerT>
        void async_read_some(BuffersT const& buffers, HandlerT&& handler)
        {
            auto slot = boost::asio::get_associated_cancellation_slot(handler);
            auto executor = boost::asio::get_associated_executor(handler, lower_.get_executor());
            lower_.async_read_some(
                buffers,
                boost::asio::bind_cancellation_slot(
                    slot,
                    boost::asio::bind_executor(
                        executor,
                        [writes = writes_, handler = std::forward<HandlerT>(handler)](
                            boost::system::error_code error, std::size_t size
                        ) mutable
                        {
                            if (writes->inFlight == 0u)
                            {
                                std::move(handler)(error, size);
                                return;
                            }
                            writes->heldRead.emplace(
                                HeldRead{.handler = std::move(handler), .error = error, .size = size}
                            );
                        }
                    )
                )
            );
        }

        template <typename BuffersT, typename HandlerT>
        void async_write_some(BuffersT const& buffers, HandlerT&& handler)
        {
            auto slot = boost::asio::get_associated_cancellation_slot(handler);
            auto executor = boost::asio::get_associated_executor(handler, lower_.get_executor());
            ++writes_->inFlight;
            lower_.async_write_some(
                buffers,
                boost::asio::bind_cancellation_slot(
                    slot,
                    boost::asio::bind_executor(
                        executor,
                        [writes = writes_, handler = std::forward<HandlerT>(handler)](
                            boost::system::error_code error, std::size_t size
                        ) mutable
                        {
                            // The TLS stream lets the next write start in here, the held read then waits for that one.
                            std::move(handler)(error, size);
                            --writes->inFlight;
                            if (writes->inFlight != 0u || !writes->heldRead)
                                return;
                            auto read = std::move(*writes->heldRead);
                            writes->heldRead.reset();
                            std::move(read.handler)(read.error, read.size);
                        }
                    )
                )
            );
        }

      private:
        struct HeldRead
        {
            boost::asio::any_completion_handler<void(boost::system::error_code, std::size_t)> handler;
            boost::system::error_code error;
            std::size_t size;
        };

        /** @brief Shared with the handlers in flight, which may outlive the stream. */
        struct Writes
        {
            std::size_t inFlight{0u};
            std::optional<HeldRead> heldRead{};
        };

        LowerT lower_;
        std::shared_ptr<Writes> writes_ = std::make_shared<Writes>();
    };
}
