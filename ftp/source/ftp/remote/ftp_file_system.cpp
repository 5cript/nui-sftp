#include <ftp/remote/ftp_file_system.hpp>
#include <paths/remote_path.hpp>
#include "../client/client_support.hpp"

#include <algorithm>
#include <optional>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Detail::errorOf;
        using RemoteIo::OpenMode;
        using RemoteIo::RemoteErrorKind;

        /**
         * @brief Which way a stream opened with @p mode transfers: FTP moves a file one way per transfer and has no
         *        exclusive create.
         */
        std::expected<FtpFileStream::Direction, RemoteIo::RemoteError> directionOf(OpenMode mode)
        {
            if (RemoteIo::contains(mode, OpenMode::Exclusive))
                return std::unexpected(
                    errorOf(RemoteErrorKind::Unsupported, "FTP cannot create a file only where none exists")
                );
            const auto reads = RemoteIo::contains(mode, OpenMode::Read);
            const auto writes = RemoteIo::contains(mode, OpenMode::Write);
            if (reads && writes)
                return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "FTP transfers a file one way at a time"));
            if (reads)
                return FtpFileStream::Direction::Download;
            if (writes)
                return FtpFileStream::Direction::Upload;
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "The file is opened neither to read nor to write"));
        }
    }

    namespace Detail
    {
        RemoteIo::RemoteError sessionEndedError()
        {
            return errorOf(RemoteErrorKind::ConnectionLost, "The FTP session ended");
        }
    }

    std::shared_ptr<FtpFileSystem> FtpFileSystem::create(
        FileSystemSession session,
        std::unique_ptr<RemoteIo::ProcessingStrand> strand,
        FtpFileSystemOptions options
    )
    {
        return std::make_shared<FtpFileSystem>(Private{}, std::move(session), std::move(strand), std::move(options));
    }

    FtpFileSystem::FtpFileSystem(
        Private,
        FileSystemSession session,
        std::unique_ptr<RemoteIo::ProcessingStrand> strand,
        FtpFileSystemOptions options
    )
        : session_{std::move(session)}
        , strand_{std::move(strand)}
        , options_{std::move(options)}
        , bufferProvider_{RemoteIo::DefaultBufferProvider::create()}
    {}

    FtpFileSystem::~FtpFileSystem() = default;

    RemoteIo::Capabilities FtpFileSystem::capabilities() const
    {
        return RemoteIo::Capabilities{
            .symlinks = false,
            .hardLinks = false,
            .setModificationTime = true,
            .exclusiveCreate = false,
            .appendWrite = true,
            .pipelinedTransfers = false,
            .canonicalPath = true,
            .userDirectories = false,
            .renameSemantics = RemoteIo::RenameSemantics::Unknown,
            .maxParallelTransfers = std::max<std::size_t>(session_.parallelTransfers, 1u),
            .preferAsyncTransfers = true,
        };
    }

    RemoteIo::ProcessingStrand* FtpFileSystem::strand() const
    {
        return strand_.get();
    }

    RemoteIo::IBufferProvider& FtpFileSystem::bufferProvider()
    {
        return *bufferProvider_;
    }

    bool FtpFileSystem::close()
    {
        if (!running_->close(closedError()))
            return false;
        const auto closeStreams = [streams = streams_]()
        {
            for (auto const& stream : streams->close())
                stream->closeInStrand();
        };
        if (strand_->withinProcessingThread())
        {
            strand_->doFinalSync(closeStreams);
            return true;
        }
        // A completion of later() closes from the session strand, which a call of a stream on the processing thread
        // waits for: waiting for that thread here would end neither.
        if (session_.engine->strand().running_in_this_thread())
        {
            strand_->pushFinalTask(closeStreams);
            return true;
        }
        // Once the thread stopped the final task runs here, no task of the session runs beside it then.
        strand_->pushFinalPromiseTask(closeStreams).get();
        return true;
    }

    void FtpFileSystem::dispatch(std::function<void()> start)
    {
        start();
    }

    void FtpFileSystem::open(
        RemoteIo::OpenFile operation,
        std::shared_ptr<TrackedCompletion<RemoteIo::OpenFile>> completion
    )
    {
        session_.engine->template run<RemoteIo::OpenFile::Result>(
            [session = session_,
                options = options_,
                strand = std::weak_ptr{strand_},
                streams = streams_,
                operation = std::move(operation)](std::shared_ptr<CallScope> const& scope) mutable
            {
                return openOn(
                    std::move(session),
                    std::move(options),
                    std::move(strand),
                    std::move(streams),
                    scope,
                    std::move(operation)
                );
            },
            options_.operationLimits,
            [completion = std::move(completion)](RemoteIo::OperationResult<RemoteIo::OpenFile> result)
            {
                completion->complete(std::move(result));
            }
        );
    }

    asio::awaitable<RemoteIo::OperationResult<RemoteIo::OpenFile>> FtpFileSystem::openOn(
        FileSystemSession session,
        FtpFileSystemOptions options,
        std::weak_ptr<RemoteIo::ProcessingStrand> strand,
        std::shared_ptr<OpenStreams> streams,
        std::shared_ptr<CallScope> scope,
        RemoteIo::OpenFile operation
    )
    {
        const auto direction = directionOf(operation.mode);
        if (!direction)
            co_return std::unexpected(direction.error());
        const auto pool = session.engine->pool();
        if (!pool)
            co_return std::unexpected(Detail::sessionEndedError());
        auto lease = co_await pool->acquireTransfer(std::move(operation.wakeWhenFree));
        if (!lease)
            co_return std::unexpected(std::move(lease).error());

        std::optional<std::uint64_t> size{};
        if (*direction == FtpFileStream::Direction::Download)
        {
            auto found = co_await Detail::onConnection<std::uint64_t>(
                lease->control(),
                std::move(scope),
                sizeOf(
                    MetadataSession{
                        .transfer = lease->transferSession(),
                        .dialect = session.dialect,
                        .options = options.transfer,
                        .limits = options.listing,
                    },
                    operation.path.value()
                )
            );
            if (found)
                size = *found;
            // Without SIZE the transfer tells a cut download by the size its first reply announces.
            else if (found.error().kind != RemoteErrorKind::Unsupported)
                co_return std::unexpected(std::move(found).error());
        }
        auto stream = FtpFileStream::create(
            std::move(session),
            std::move(options),
            std::move(strand),
            std::move(*lease),
            std::move(operation.path),
            *direction,
            size,
            streams
        );
        // Dropped, the stream gives its connection back.
        if (!streams->add(stream))
            co_return std::unexpected(closedError());
        co_return std::weak_ptr<RemoteIo::IRemoteFileStream>{stream};
    }

    RemoteIo::OperationResult<RemoteIo::HomeDirectory> FtpFileSystem::answer(RemoteIo::HomeDirectory const&) const
    {
        return Paths::RemotePath::make(session_.homeDirectory).value_or(Paths::RemotePath::root());
    }

    RemoteIo::OperationResult<RemoteIo::CanonicalHomeDirectory>
    FtpFileSystem::answer(RemoteIo::CanonicalHomeDirectory const&) const
    {
        // PWD names the directory as the server resolves it.
        return Paths::RemotePath::make(session_.homeDirectory)
            .transform_error(
                [](Paths::PathError const&)
                {
                    return errorOf(RemoteErrorKind::Failure, "The server named no usable home directory");
                }
            );
    }

    RemoteIo::OperationResult<RemoteIo::ReadTransferLimits>
    FtpFileSystem::answer(RemoteIo::ReadTransferLimits const&) const
    {
        return RemoteIo::TransferLimits{};
    }

    RemoteIo::OperationResult<RemoteIo::CreateSymLink> FtpFileSystem::answer(RemoteIo::CreateSymLink const&) const
    {
        return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "FTP has no command to create a symbolic link"));
    }

    RemoteIo::RemoteError FtpFileSystem::closedError()
    {
        return errorOf(RemoteErrorKind::ConnectionLost, "The FTP file system was closed");
    }
}
