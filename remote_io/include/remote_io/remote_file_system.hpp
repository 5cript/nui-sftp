#pragma once

#include <remote_io/async/buffer_provider.hpp>
#include <remote_io/async/processing_strand.hpp>
#include <remote_io/async/unfinished_task_result.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system_operations.hpp>

#include <array>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace RemoteIo
{
    /**
     * @brief What a rename does to an existing destination.
     */
    enum class RenameSemantics
    {
        Replaces,
        Refuses,
        // The server does not tell, a rename may do either.
        Unknown,
    };

    /**
     * @brief What a remote file system can do, fixed once it is connected.
     */
    struct Capabilities
    {
        bool symlinks{false};
        /** @brief LinkIntoPlace can move a file through a hard link. */
        bool hardLinks{false};
        bool setModificationTime{false};
        /** @brief Opening with OpenMode::Exclusive fails when the file exists. */
        bool exclusiveCreate{false};
        /** @brief Writing after a seek to the end continues a file. */
        bool appendWrite{false};
        /** @brief Several reads or writes of one file can be in flight at once. */
        bool pipelinedTransfers{false};
        bool canonicalPath{false};
        bool userDirectories{false};
        RenameSemantics renameSemantics{RenameSemantics::Unknown};
        /** @brief How many transfers can run at once, nothing for no limit. */
        std::optional<std::size_t> maxParallelTransfers{};
        /** @brief Small files also take the asynchronous transfer path. */
        bool preferAsyncTransfers{false};
        /**
         * @brief ProbeEntry answers. Without it nothing checks what a write would replace, short of an Lstat that may
         *        list a whole directory.
         */
        bool probeEntries{false};
    };

    /**
     * @brief Tells a waiting thread that an operation completed.
     */
    class CompletionSignal
    {
      public:
        void notify()
        {
            {
                std::scoped_lock lock{mutex_};
                completed_ = true;
            }
            condition_.notify_all();
        }

        bool completed() const
        {
            std::scoped_lock lock{mutex_};
            return completed_;
        }

        void wait()
        {
            std::unique_lock lock{mutex_};
            condition_.wait(
                lock,
                [this]()
                {
                    return completed_;
                }
            );
        }

      private:
        mutable std::mutex mutex_{};
        std::condition_variable condition_{};
        bool completed_{false};
    };

    /**
     * @brief A file system on a server, whatever protocol it speaks. Every operation of RemoteFileSystemOperations can
     *        run on the processing thread of its strand with @ref inStrand, or from any thread with @ref later.
     *        Implementations derive from RemoteFileSystemImplementation and complete every operation they start, if
     *        need be as timed out, so no caller waits forever and no result arrives after its caller left.
     */
    class IRemoteFileSystem
    {
      public:
        using Trampoline = void (*)(IRemoteFileSystem& fileSystem, void* pending);
        using Trampolines = std::array<Trampoline, RemoteFileSystemOperations::size>;

        virtual ~IRemoteFileSystem() = default;
        IRemoteFileSystem(IRemoteFileSystem const&) = delete;
        IRemoteFileSystem& operator=(IRemoteFileSystem const&) = delete;
        IRemoteFileSystem(IRemoteFileSystem&&) = delete;
        IRemoteFileSystem& operator=(IRemoteFileSystem&&) = delete;

        /**
         * @brief Runs @p operation and returns its result. Must be called on the processing thread of the strand.
         */
        template <typename OperationT>
        OperationResult<OperationT> inStrand(OperationT operation)
        {
            assert(strand()->withinProcessingThread());
            if (hasUnsetPath(operation))
                return std::unexpected(unsetPathError());
            // The completing thread may still be inside notify when the wait returns, so the state is shared.
            const auto call = std::make_shared<InStrandCall<OperationResult<OperationT>>>();
            start(
                Pending<OperationT>{
                    .operation = std::move(operation),
                    .complete = Completion<OperationT>(
                        [call](OperationResult<OperationT> result)
                        {
                            call->result = std::move(result);
                            call->completed.notify();
                        }
                    ),
                }
            );
            if (!call->completed.completed())
                awaitInStrand(call->completed);
            assert(call->completed.completed());
            return std::move(*call->result);
        }

        /**
         * @brief Runs @p operation from any thread and calls @p onCompleted once with its result, on the thread that
         *        completes it: the processing thread, a thread of the protocol, or the caller's when the operation
         *        fails before it starts. The file system must be owned by a shared_ptr: when it is gone before the
         *        operation starts, the operation completes as unfinished. Separate calls run in order only where the
         *        protocol starts operations on the strand, a caller that needs one after another starts it from the
         *        first one's completion.
         */
        template <typename OperationT, typename CallbackT>
        void later(OperationT operation, CallbackT&& onCompleted)
        {
            Completion<OperationT> completion{
                sharedCallback<OperationResult<OperationT>>(std::forward<CallbackT>(onCompleted))
            };
            if (hasUnsetPath(operation))
                return completion(std::unexpected(unsetPathError()));
            auto pending = std::make_shared<Pending<OperationT>>(std::move(operation), std::move(completion));
            dispatchWhileOwned(
                [pending = std::move(pending)](IRemoteFileSystem& fileSystem)
                {
                    fileSystem.start(std::move(*pending));
                }
            );
        }

        /**
         * @brief Runs @p operation from any thread, the future holds its result, see later(operation, onCompleted).
         */
        template <typename OperationT>
        std::future<OperationResult<OperationT>> later(OperationT operation)
        {
            auto promise = std::make_shared<std::promise<OperationResult<OperationT>>>();
            auto future = promise->get_future();
            later(
                std::move(operation),
                [promise = std::move(promise)](OperationResult<OperationT> result)
                {
                    promise->set_value(std::move(result));
                }
            );
            return future;
        }

        template <typename FunctionT>
        void perform(FunctionT&& function)
        {
            strand()->pushTask(std::forward<FunctionT>(function));
        }

        /**
         * @brief Runs @p function on the processing thread of the strand and calls @p onCompleted once with what it
         *        returned, there, or with the unfinished result, see UnfinishedTaskResult, where the strand refuses or
         *        drops the task. It is not ordered with operations that later() starts outside the strand.
         */
        template <typename FunctionT, typename CallbackT>
        void perform(FunctionT&& function, CallbackT&& onCompleted)
        {
            using ResultT = std::invoke_result_t<std::decay_t<FunctionT>&>;
            auto completion = std::make_shared<ResultCompletion<ResultT>>(
                sharedCallback<ResultT>(std::forward<CallbackT>(onCompleted))
            );
            // A refused task is destroyed at once, a dropped one with the thread's queue: both complete as unfinished.
            strand()->pushTask(
                [function = std::forward<FunctionT>(function), completion = std::move(completion)]() mutable
                {
                    (*completion)(function());
                }
            );
        }

        template <
            typename FunctionT,
            typename UnfinishedFunc = UnfinishedTaskResult<std::invoke_result_t<std::decay_t<FunctionT>>>>
        auto performPromise(FunctionT&& function, UnfinishedFunc unfinished = {})
            -> std::future<std::invoke_result_t<std::decay_t<FunctionT>>>
        {
            return strand()->pushPromiseTask(std::forward<FunctionT>(function), std::move(unfinished));
        }

        virtual Capabilities capabilities() const = 0;
        virtual ProcessingStrand* strand() const = 0;
        /**
         * @brief The buffer pool the transfers of this file system share.
         */
        virtual IBufferProvider& bufferProvider() = 0;

        /**
         * @brief Closes the file system and every stream of it.
         * @return false when it was closed already.
         */
        virtual bool close() = 0;

      protected:
        explicit IRemoteFileSystem(Trampolines const& trampolines)
            : trampolines_{&trampolines}
        {}

        /**
         * @brief Runs @p start where operations begin that @ref later started: on the strand, or at once where the
         *        protocol needs no strand to begin one. Dropping @p start completes its operation as unfinished.
         */
        virtual void dispatch(std::function<void()> start) = 0;

        /**
         * @brief Waits on the processing thread until an operation that did not complete when it started completed.
         *        Never returns earlier: the protocol bounds the operation and completes it as timed out itself.
         *        Protocols that make progress only on this thread override it to drive themselves meanwhile.
         */
        virtual void awaitInStrand(CompletionSignal& completed)
        {
            completed.wait();
        }

      private:
        /**
         * @brief Runs @p start through @ref dispatch while a shared_ptr still owns this file system, drops it when none
         *        does.
         */
        virtual void dispatchWhileOwned(std::function<void(IRemoteFileSystem&)> start) = 0;

        /**
         * @brief @p callback as the copyable function a completion holds, shared when it can only move.
         */
        template <typename ResultT, typename CallbackT>
        static std::function<void(ResultT)> sharedCallback(CallbackT&& callback)
        {
            if constexpr (std::is_copy_constructible_v<std::decay_t<CallbackT>>)
                return std::forward<CallbackT>(callback);
            else
            {
                auto shared = std::make_shared<std::decay_t<CallbackT>>(std::forward<CallbackT>(callback));
                return [shared = std::move(shared)](ResultT result)
                {
                    (*shared)(std::move(result));
                };
            }
        }

        template <typename ResultT>
        struct InStrandCall
        {
            CompletionSignal completed{};
            std::optional<ResultT> result{};
        };

        template <typename OperationT>
        void start(Pending<OperationT>&& pending)
        {
            (*trampolines_)[RemoteFileSystemOperations::indexOf<OperationT>()](*this, &pending);
        }

        static RemoteError unsetPathError()
        {
            return RemoteError{
                .kind = RemoteErrorKind::Failure,
                .message = "The remote path is not set",
                .wrapperError = WrapperErrors::InvalidPath,
            };
        }

        Trampolines const* trampolines_;
    };

    /**
     * @brief The base of a remote file system implementation. @p DerivedT starts every operation of
     *        RemoteFileSystemOperations with a member template `void startOperation(Pending<OperationT>)`, which
     *        completes it now or later; a missing operation does not compile. @p DerivedT gets shared_from_this and
     *        weak_from_this of its own type from this base.
     */
    template <typename DerivedT>
    class RemoteFileSystemImplementation
        : public IRemoteFileSystem
        , public std::enable_shared_from_this<DerivedT>
    {
      protected:
        RemoteFileSystemImplementation()
            : IRemoteFileSystem{trampolines}
        {
            static_assert(
                std::is_convertible_v<DerivedT*, std::enable_shared_from_this<DerivedT>*>,
                "Use the enable_shared_from_this base of RemoteFileSystemImplementation, a second one is never set"
            );
        }

      private:
        void dispatchWhileOwned(std::function<void(IRemoteFileSystem&)> start) final
        {
            this->dispatch(
                [owner = this->weak_from_this(), start = std::move(start)]()
                {
                    if (const auto locked = owner.lock())
                        start(*locked);
                }
            );
        }

        template <typename OperationT>
        static void trampoline(IRemoteFileSystem& fileSystem, void* pending)
        {
            static_cast<DerivedT&>(fileSystem).startOperation(std::move(*static_cast<Pending<OperationT>*>(pending)));
        }

        template <typename... OperationTs>
        static constexpr Trampolines trampolinesOf(OperationList<OperationTs...>)
        {
            return Trampolines{&trampoline<OperationTs>...};
        }

        static constexpr Trampolines trampolines = trampolinesOf(RemoteFileSystemOperations{});
    };
}
