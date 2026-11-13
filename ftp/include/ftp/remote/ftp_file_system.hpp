#pragma once

#include <ftp/engine/call.hpp>
#include <ftp/engine/engine.hpp>
#include <ftp/remote/file_system_session.hpp>
#include <ftp/remote/ftp_file_stream.hpp>
#include <ftp/remote/running_operations.hpp>
#include <remote_io/async/buffer_provider.hpp>
#include <remote_io/async/processing_strand.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system.hpp>

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace FileTransferProtocol
{
    /**
     * @brief The files of an FTP server as a RemoteIo file system, one channel of a session.
     *
     * Operations start at once on the engine of the session, from any thread, and each takes a connection for
     * commands for as long as it runs: C0, an idle transfer connection, or C0 in turn. OpenFile takes a transfer
     * connection for the stream instead, and fails with Busy while every one transfers. The engine bounds each
     * operation with the operation limits and completes it on the session strand, as TimedOut if need be, so
     * inStrand() waits on the processing thread without driving anything. Operations that need no server answer at
     * once.
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
         * @brief Fails the operations under way with ConnectionLost and refuses later ones, then closes the streams in
         *        a final task, which finalizes the strand, and waits for that task. Not on the session strand, where
         *        the completions of later() run: the streams close behind what the processing thread does then. An
         *        operation that was failed still runs to its end on the engine, its result is dropped.
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
            if constexpr (std::is_same_v<OperationT, RemoteIo::OpenFile>)
                open(std::move(pending.operation), std::move(completion));
            else if constexpr (RunsOnACommandConnection<OperationT>)
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
                    return Detail::onCommandConnection(
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

        void open(RemoteIo::OpenFile operation, std::shared_ptr<TrackedCompletion<RemoteIo::OpenFile>> completion);

        /**
         * @brief Takes a transfer connection for a stream and asks a file to read for its size, which a server
         *        without SIZE does not tell, on the session strand.
         */
        static boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::OpenFile>> openOn(
            FileSystemSession session,
            FtpFileSystemOptions options,
            std::weak_ptr<RemoteIo::ProcessingStrand> strand,
            std::shared_ptr<OpenStreams> streams,
            std::shared_ptr<CallScope> scope,
            RemoteIo::OpenFile operation
        );

        RemoteIo::OperationResult<RemoteIo::HomeDirectory> answer(RemoteIo::HomeDirectory const& operation) const;
        RemoteIo::OperationResult<RemoteIo::CanonicalHomeDirectory>
        answer(RemoteIo::CanonicalHomeDirectory const& operation) const;
        RemoteIo::OperationResult<RemoteIo::ReadTransferLimits>
        answer(RemoteIo::ReadTransferLimits const& operation) const;
        RemoteIo::OperationResult<RemoteIo::CreateSymLink> answer(RemoteIo::CreateSymLink const& operation) const;

        static RemoteIo::RemoteError closedError();

        const FileSystemSession session_;
        /** @brief Shared so that a stream finds out whether it is still there. */
        const std::shared_ptr<RemoteIo::ProcessingStrand> strand_;
        const FtpFileSystemOptions options_;
        const std::shared_ptr<RunningOperations> running_ = std::make_shared<RunningOperations>();
        const std::shared_ptr<OpenStreams> streams_ = std::make_shared<OpenStreams>();
        const std::shared_ptr<RemoteIo::IBufferProvider> bufferProvider_;
    };
}
