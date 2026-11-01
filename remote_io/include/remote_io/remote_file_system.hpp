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
    class IRemoteFileSystem : public std::enable_shared_from_this<IRemoteFileSystem>
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
            auto complete = Completion<OperationT>(
                [call](OperationResult<OperationT> result)
                {
                    call->result = std::move(result);
                    call->completed.notify();
                }
            );
            start(Pending<OperationT>{.operation = std::move(operation), .complete = std::move(complete)});
            if (!call->completed.completed())
                awaitInStrand(call->completed);
            assert(call->completed.completed());
            return std::move(*call->result);
        }

        /**
         * @brief Runs @p operation from any thread, the future holds its result.
         */
        template <typename OperationT>
        std::future<OperationResult<OperationT>> later(OperationT operation)
        {
            auto promise = std::make_shared<std::promise<OperationResult<OperationT>>>();
            auto future = promise->get_future();
            if (hasUnsetPath(operation))
            {
                promise->set_value(std::unexpected(unsetPathError()));
                return future;
            }
            auto complete = Completion<OperationT>(
                [promise](OperationResult<OperationT> result)
                {
                    promise->set_value(std::move(result));
                }
            );
            auto pending = std::make_shared<Pending<OperationT>>(
                Pending<OperationT>{.operation = std::move(operation), .complete = std::move(complete)}
            );
            dispatch(
                [this, pending]()
                {
                    start(std::move(*pending));
                }
            );
            return future;
        }

        template <typename FunctionT>
        void perform(FunctionT&& function)
        {
            strand()->pushTask(std::forward<FunctionT>(function));
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
        template <typename ResultT>
        struct InStrandCall
        {
            CompletionSignal completed{};
            std::optional<ResultT> result{};
        };

        template <typename OperationT>
        void start(Pending<OperationT> pending)
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
     *        completes it now or later; a missing operation does not compile.
     */
    template <typename DerivedT>
    class RemoteFileSystemImplementation : public IRemoteFileSystem
    {
      protected:
        RemoteFileSystemImplementation()
            : IRemoteFileSystem{trampolines}
        {}

      private:
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
