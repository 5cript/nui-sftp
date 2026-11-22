#pragma once

#include <remote_io/remote_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
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
     * @brief What an operation tells the engine that runs it: the control connections it used, closed when it does
     *        not end after its cancel, and the time it needs beyond its backstop. Used on the session strand.
     */
    class CallScope
    {
      public:
        using AllowMore = std::function<void(std::chrono::milliseconds more)>;

        void touch(std::shared_ptr<ControlConnection> const& connection);

        /**
         * @brief Closes every touched connection that still exists.
         */
        void closeTouched();

        /**
         * @brief Moves the backstop of the operation later by @p more, for one that learns on its way how much it has
         *        to do. Nothing once the operation was cancelled or ended.
         */
        void allowMore(std::chrono::milliseconds more);

        /**
         * @brief What allowMore() calls: the engine that runs the operation sets it.
         */
        void onAllowMore(AllowMore allowMore);

      private:
        std::vector<std::weak_ptr<ControlConnection>> touched_{};
        AllowMore allowMore_{};
    };

    /**
     * @brief Synchronous calls that end together before their time: the close of a file system ends the calls its
     *        streams wait in. Thread-safe.
     */
    class CallGroup
    {
      public:
        /** @brief Ends one waiting call: completes its wait with the error and cancels its operation. */
        using Ender = std::function<void(RemoteIo::RemoteError const&)>;

        CallGroup() = default;
        ~CallGroup() = default;
        CallGroup(CallGroup const&) = delete;
        CallGroup& operator=(CallGroup const&) = delete;
        CallGroup(CallGroup&&) = delete;
        CallGroup& operator=(CallGroup&&) = delete;

        /**
         * @brief Ends every call waiting now with @p error, outside the lock, and every later one at once. Only the
         *        first end counts.
         */
        void end(RemoteIo::RemoteError error);

        /**
         * @brief Registers the call that @p ender ends.
         *
         * @return Its key, nothing once the group ended: the caller ends its call with endedWith() then.
         */
        std::optional<std::uint64_t> join(Ender ender);

        void leave(std::uint64_t key);

        /**
         * @return The error of end(), nothing before it.
         */
        std::optional<RemoteIo::RemoteError> endedWith() const;

      private:
        mutable std::mutex mutex_{};
        std::unordered_map<std::uint64_t, Ender> waiting_{};
        std::uint64_t nextKey_{0u};
        std::optional<RemoteIo::RemoteError> ended_{};
    };

    namespace Detail
    {
        /**
         * @brief The result a synchronous call waits for, set once by whichever comes first: the operation or the end
         *        of its group. Thread-safe.
         */
        template <typename ValueT>
        class SyncOutcome
        {
          public:
            using Result = std::expected<ValueT, RemoteIo::RemoteError>;

            std::future<Result> future()
            {
                return promise_.get_future();
            }

            void set(Result result)
            {
                std::scoped_lock lock{mutex_};
                if (std::exchange(set_, true))
                    return;
                promise_.set_value(std::move(result));
            }

          private:
            std::mutex mutex_{};
            bool set_{false};
            std::promise<Result> promise_{};
        };

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

            /**
             * @brief The bounds of the operation, the backstop with what extendBackstop() added.
             */
            SyncLimits const& limits() const;
            void extendBackstop(std::chrono::milliseconds more);
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
            SyncLimits limits_;
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
