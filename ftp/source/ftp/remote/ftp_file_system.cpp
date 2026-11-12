#include <ftp/remote/ftp_file_system.hpp>
#include <paths/remote_path.hpp>
#include "../client/client_support.hpp"

#include <algorithm>

namespace FileTransferProtocol
{
    using Detail::errorOf;
    using RemoteIo::RemoteErrorKind;

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
        if (strand_->isFinalized())
            return true;
        if (!strand_->withinProcessingThread())
        {
            // A refused or dropped final task leaves nothing to wait for.
            strand_->pushFinalPromiseTask([]() {}).wait();
            return true;
        }
        strand_->doFinalSync([]() {});
        return true;
    }

    void FtpFileSystem::dispatch(std::function<void()> start)
    {
        start();
    }

    RemoteIo::OperationResult<RemoteIo::OpenFile> FtpFileSystem::answer(RemoteIo::OpenFile const&) const
    {
        return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "FTP file streams are not there yet"));
    }

    RemoteIo::OperationResult<RemoteIo::ReadFile> FtpFileSystem::answer(RemoteIo::ReadFile const&) const
    {
        return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "FTP file streams are not there yet"));
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

    RemoteIo::RemoteError FtpFileSystem::sessionEndedError()
    {
        return errorOf(RemoteErrorKind::ConnectionLost, "The FTP session ended");
    }
}
