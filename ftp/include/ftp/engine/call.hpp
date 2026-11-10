#pragma once

#include <remote_io/remote_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace FileTransferProtocol
{
    class ControlConnection;

    /**
     * @brief The bounds of one operation the engine runs for a caller. The operations bound their own waits, the
     *        backstop is only the guarantee that the caller gets an answer.
     */
    struct SyncLimits
    {
        /** @brief From the start of the operation to its cancel. */
        std::chrono::milliseconds backstop{std::chrono::seconds{30}};
        /** @brief From the cancel to giving up on the operation, which closes the connections it used. */
        std::chrono::milliseconds drain{std::chrono::seconds{1}};
    };

    /**
     * @brief The control connections an operation used, closed when it does not end after its cancel. Used on the
     *        session strand.
     */
    class CallScope
    {
      public:
        void touch(std::shared_ptr<ControlConnection> const& connection);

        /**
         * @brief Closes every touched connection that still exists.
         */
        void closeTouched();

      private:
        std::vector<std::weak_ptr<ControlConnection>> touched_{};
    };

    namespace Detail
    {
        /**
         * @brief What an operation the engine runs needs besides its value: the signal that cancels it, its timer and
         *        its scope. Owned by the handlers of the operation, used on the session strand.
         */
        class CallState
        {
          public:
            CallState(boost::asio::any_io_executor const& sessionStrand, SyncLimits limits);
            virtual ~CallState() = default;
            CallState(CallState const&) = delete;
            CallState& operator=(CallState const&) = delete;
            CallState(CallState&&) = delete;
            CallState& operator=(CallState&&) = delete;

            SyncLimits const& limits() const;
            std::shared_ptr<CallScope> const& scope() const;
            boost::asio::cancellation_slot slot();
            boost::asio::steady_timer& timer();

            /** @brief The engine's key of the operation while it runs. */
            std::uint64_t number{0u};

            /**
             * @brief Cancels the operation, terminally. From now on it drains.
             */
            void cancel();
            bool draining() const;

            /**
             * @brief Completes the operation with @p error, does nothing once it completed.
             */
            virtual void fail(RemoteIo::RemoteError error) = 0;
            virtual bool completed() const = 0;

          private:
            const SyncLimits limits_;
            const std::shared_ptr<CallScope> scope_ = std::make_shared<CallScope>();
            boost::asio::cancellation_signal signal_{};
            boost::asio::steady_timer timer_;
            bool draining_{false};
        };

        /**
         * @brief An operation that completes once, as lost when it is dropped before.
         */
        template <typename ValueT>
        class Call final : public CallState
        {
          public:
            using Result = std::expected<ValueT, RemoteIo::RemoteError>;

            Call(boost::asio::any_io_executor const& sessionStrand, SyncLimits limits, std::function<void(Result)> done)
                : CallState{sessionStrand, limits}
                , done_{std::move(done)}
            {}

            ~Call() override
            {
                fail(
                    RemoteIo::RemoteError{
                        .kind = RemoteIo::RemoteErrorKind::ConnectionLost,
                        .protocol = RemoteIo::Protocol::Ftp,
                        .message = "The FTP session dropped the operation",
                    }
                );
            }
            Call(Call const&) = delete;
            Call& operator=(Call const&) = delete;
            Call(Call&&) = delete;
            Call& operator=(Call&&) = delete;

            void complete(Result result)
            {
                if (!done_)
                    return;
                const auto done = std::exchange(done_, nullptr);
                done(std::move(result));
            }

            void fail(RemoteIo::RemoteError error) override
            {
                complete(std::unexpected(std::move(error)));
            }

            bool completed() const override
            {
                return !done_;
            }

          private:
            std::function<void(Result)> done_;
        };
    }
}
