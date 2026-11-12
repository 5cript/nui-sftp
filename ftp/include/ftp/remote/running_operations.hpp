#pragma once

#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system_operations.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace FileTransferProtocol
{
    /**
     * @brief An operation that close() of its file system can end.
     */
    class FailableOperation
    {
      public:
        FailableOperation() = default;
        virtual ~FailableOperation() = default;
        FailableOperation(FailableOperation const&) = delete;
        FailableOperation& operator=(FailableOperation const&) = delete;
        FailableOperation(FailableOperation&&) = delete;
        FailableOperation& operator=(FailableOperation&&) = delete;

        /**
         * @brief Completes the operation with @p error unless it completed already. Any thread.
         */
        virtual void fail(RemoteIo::RemoteError error) = 0;
    };

    /**
     * @brief The operations a file system started that did not complete yet, so its close completes them. Holds them
     *        weakly, an operation that is gone has completed. Thread-safe.
     */
    class RunningOperations : public std::enable_shared_from_this<RunningOperations>
    {
      public:
        /**
         * @brief Registers @p operation.
         *
         * @return Its key, nothing once closed.
         */
        std::optional<std::uint64_t> add(std::weak_ptr<FailableOperation> operation);

        void remove(std::uint64_t key);

        /**
         * @brief Fails every registered operation with @p error, outside the lock, and refuses later ones.
         *
         * @return False when it was closed already.
         */
        bool close(RemoteIo::RemoteError const& error);

        bool closed() const;

        /**
         * @brief How many operations are registered.
         */
        std::size_t size() const;

      private:
        mutable std::mutex mutex_{};
        std::unordered_map<std::uint64_t, std::weak_ptr<FailableOperation>> running_{};
        std::uint64_t nextKey_{0u};
        bool closed_{false};
    };

    /**
     * @brief The completion of one operation, called once by whichever comes first: the engine with the result, or
     *        the close of the file system with its error. Removes itself from RunningOperations then. Thread-safe.
     */
    template <typename OperationT>
    class TrackedCompletion final : public FailableOperation
    {
      public:
        using Result = RemoteIo::OperationResult<OperationT>;

        explicit TrackedCompletion(RemoteIo::Completion<OperationT> completion)
            : completion_{std::in_place, std::move(completion)}
        {}

        /**
         * @brief Unregisters it. A completion that was never called completes as unfinished.
         */
        ~TrackedCompletion() override
        {
            if (const auto running = running_.lock(); running && key_)
                running->remove(*key_);
        }
        TrackedCompletion(TrackedCompletion const&) = delete;
        TrackedCompletion& operator=(TrackedCompletion const&) = delete;
        TrackedCompletion(TrackedCompletion&&) = delete;
        TrackedCompletion& operator=(TrackedCompletion&&) = delete;

        /**
         * @brief Registers it in @p running.
         *
         * @return False once @p running is closed, the completion has the caller's error to get then.
         */
        bool track(std::shared_ptr<RunningOperations> const& running, std::shared_ptr<TrackedCompletion> const& self)
        {
            const auto key = running->add(self);
            if (!key)
                return false;
            bool completed = false;
            {
                std::scoped_lock lock{mutex_};
                running_ = running;
                key_ = key;
                completed = !completion_;
            }
            // A close between add() and here failed it already.
            if (completed)
                running->remove(*key);
            return true;
        }

        void complete(Result result)
        {
            std::optional<RemoteIo::Completion<OperationT>> taken{};
            std::weak_ptr<RunningOperations> running{};
            std::optional<std::uint64_t> key{};
            {
                std::scoped_lock lock{mutex_};
                if (!completion_)
                    return;
                taken.emplace(std::move(*completion_));
                completion_.reset();
                running = std::exchange(running_, {});
                key = std::exchange(key_, std::nullopt);
            }
            if (const auto locked = running.lock(); locked && key)
                locked->remove(*key);
            (*taken)(std::move(result));
        }

        void fail(RemoteIo::RemoteError error) override
        {
            complete(std::unexpected(std::move(error)));
        }

      private:
        std::mutex mutex_{};
        std::optional<RemoteIo::Completion<OperationT>> completion_;
        std::weak_ptr<RunningOperations> running_{};
        std::optional<std::uint64_t> key_{};
    };
}
