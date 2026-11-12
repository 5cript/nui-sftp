#pragma once

#include <ftp/client/metadata.hpp>
#include <ftp/client/transfer.hpp>
#include <ftp/engine/call.hpp>
#include <ftp/engine/engine.hpp>
#include <ftp/protocol/listing.hpp>
#include <ftp/remote/file_system_operations.hpp>
#include <ftp/remote/running_operations.hpp>
#include <remote_io/async/buffer_provider.hpp>
#include <remote_io/async/processing_strand.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace FileTransferProtocol
{
    struct FtpFileSystemOptions
    {
        /** @brief The bounds of each operation, see Engine::run(). */
        SyncLimits operationLimits{};
        TransferOptions transfer{};
        ListingLimits listing{};
    };

    /**
     * @brief What the file systems of one session share, fixed once its first login told it.
     */
    struct FileSystemSession
    {
        std::shared_ptr<Engine> engine{};
        /** @brief What the session learned about the commands of its server. */
        std::shared_ptr<ServerDialect> dialect = std::make_shared<ServerDialect>();
        /** @brief The directory the first login started in, from PWD, empty when the server did not tell. */
        std::string homeDirectory{};
        /** @brief How many transfers run at once, the transfer connections of the pool. */
        std::size_t parallelTransfers{1u};
    };

    /**
     * @brief An operation FtpFileSystem runs with the commands of a connection of its pool.
     */
    template <typename OperationT>
    concept RunsOnACommandConnection = requires(MetadataSession session, OperationT operation) {
        runOperation(std::move(session), std::move(operation));
    };

    /**
     * @brief The files of an FTP server as a RemoteIo file system, one channel of a session.
     *
     * Operations start at once on the engine of the session, from any thread, and each takes a connection for
     * commands for as long as it runs: C0, an idle transfer connection, or C0 in turn. The engine bounds each one
     * with the operation limits and completes it on the session strand, as TimedOut if need be, so inStrand() waits
     * on the processing thread without driving anything. Operations that need no server answer at once.
     */
    class FtpFileSystem : public RemoteIo::RemoteFileSystemImplementation<FtpFileSystem>
    {
        struct Private
        {};
        friend class RemoteIo::RemoteFileSystemImplementation<FtpFileSystem>;

      public:
        /**
         * @param strand The strand of this file system on the ProcessingThread of the session.
         */
        static std::shared_ptr<FtpFileSystem> create(
            FileSystemSession session,
            std::unique_ptr<RemoteIo::ProcessingStrand> strand,
            FtpFileSystemOptions options = {}
        );

        FtpFileSystem(
            Private,
            FileSystemSession session,
            std::unique_ptr<RemoteIo::ProcessingStrand> strand,
            FtpFileSystemOptions options
        );
        ~FtpFileSystem() override;
        FtpFileSystem(FtpFileSystem const&) = delete;
        FtpFileSystem& operator=(FtpFileSystem const&) = delete;
        FtpFileSystem(FtpFileSystem&&) = delete;
        FtpFileSystem& operator=(FtpFileSystem&&) = delete;

        /**
         * @brief No symlinks to create, which FTP has no command for, but ReadLink and ReadLinkDeep read the targets
         *        LIST names. Renames replace on the servers tested, others may refuse.
         */
        RemoteIo::Capabilities capabilities() const override;
        RemoteIo::ProcessingStrand* strand() const override;
        RemoteIo::IBufferProvider& bufferProvider() override;

        /**
         * @brief Fails the operations under way with ConnectionLost and refuses later ones, then finalizes the strand.
         *        An operation that was failed still runs to its end on the engine, its result is dropped.
         *
         * @return False when it was closed already.
         */
        bool close() override;

      protected:
        /**
         * @brief Runs @p start at once: operations run on the engine, so browsing never waits behind the tasks of the
         *        session.
         */
        void dispatch(std::function<void()> start) override;

      private:
        template <typename OperationT>
        void startOperation(RemoteIo::Pending<OperationT> pending)
        {
            auto completion = std::make_shared<TrackedCompletion<OperationT>>(std::move(pending.complete));
            if (!completion->track(running_, completion))
            {
                completion->fail(closedError());
                return;
            }
            if constexpr (RunsOnACommandConnection<OperationT>)
                runOnServer(std::move(pending.operation), std::move(completion));
            else
                completion->complete(answer(pending.operation));
        }

        template <typename OperationT>
        void runOnServer(OperationT operation, std::shared_ptr<TrackedCompletion<OperationT>> completion)
        {
            using ValueT = typename OperationT::Result;
            session_.engine->template run<ValueT>(
                [session = session_, options = options_, operation = std::move(operation)](
                    std::shared_ptr<CallScope> const& scope
                ) mutable
                {
                    return onCommandConnection<ValueT>(
                        std::move(session), std::move(options), scope, std::move(operation)
                    );
                },
                options_.operationLimits,
                [completion = std::move(completion)](Engine::Result<ValueT> result)
                {
                    completion->complete(std::move(result));
                }
            );
        }

        /**
         * @brief Takes a connection for commands and runs @p operation on its strand, on the session strand.
         */
        template <typename ValueT, typename OperationT>
        static boost::asio::awaitable<Engine::Result<ValueT>> onCommandConnection(
            FileSystemSession session,
            FtpFileSystemOptions options,
            std::shared_ptr<CallScope> scope,
            OperationT operation
        )
        {
            const auto pool = session.engine->pool();
            if (!pool)
                co_return std::unexpected(sessionEndedError());
            auto lease = co_await pool->acquireCommand();
            if (!lease)
                co_return std::unexpected(std::move(lease).error());
            const auto control = lease->control();
            scope->touch(control);
            auto [exception, result] = co_await boost::asio::co_spawn(
                control->strand(),
                runWithoutThrowing(
                    MetadataSession{
                        .transfer = lease->transferSession(),
                        .dialect = std::move(session.dialect),
                        .options = options.transfer,
                        .limits = options.listing,
                    },
                    std::move(operation)
                ),
                boost::asio::as_tuple(boost::asio::use_awaitable)
            );
            // Fails the session, see Engine::run().
            if (exception)
                std::rethrow_exception(exception);
            co_return std::move(result);
        }

        template <typename OperationT>
        static boost::asio::awaitable<RemoteIo::OperationResult<OperationT>>
        runWithoutThrowing(MetadataSession session, OperationT operation)
        {
            co_await boost::asio::this_coro::throw_if_cancelled(false);
            co_return co_await runOperation(std::move(session), std::move(operation));
        }

        RemoteIo::OperationResult<RemoteIo::OpenFile> answer(RemoteIo::OpenFile const& operation) const;
        RemoteIo::OperationResult<RemoteIo::ReadFile> answer(RemoteIo::ReadFile const& operation) const;
        RemoteIo::OperationResult<RemoteIo::HomeDirectory> answer(RemoteIo::HomeDirectory const& operation) const;
        RemoteIo::OperationResult<RemoteIo::CanonicalHomeDirectory>
        answer(RemoteIo::CanonicalHomeDirectory const& operation) const;
        RemoteIo::OperationResult<RemoteIo::ReadTransferLimits>
        answer(RemoteIo::ReadTransferLimits const& operation) const;
        RemoteIo::OperationResult<RemoteIo::CreateSymLink> answer(RemoteIo::CreateSymLink const& operation) const;

        static RemoteIo::RemoteError closedError();
        static RemoteIo::RemoteError sessionEndedError();

        const FileSystemSession session_;
        const std::unique_ptr<RemoteIo::ProcessingStrand> strand_;
        const FtpFileSystemOptions options_;
        const std::shared_ptr<RunningOperations> running_ = std::make_shared<RunningOperations>();
        const std::shared_ptr<RemoteIo::IBufferProvider> bufferProvider_;
    };
}
