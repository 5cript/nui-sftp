#pragma once

#include <ftp/client/metadata.hpp>
#include <remote_io/remote_file_system_operations.hpp>

#include <boost/asio/awaitable.hpp>

namespace FileTransferProtocol
{
    /**
     * @brief The operations of RemoteIo that need the server and no transfer of file content, built on the metadata
     *        operations. Each runs on the strand of the control connection of @p session, which it has to itself
     *        until it ended, and ends as Cancelled after a cancel without asking the server more. Run them with
     *        throw_if_cancelled(false), as FtpFileSystem does: a cancel that no operation under way took must not
     *        throw at the next step.
     *
     * ListDirectory takes the end of a link that names an entry of the same listing from that listing, with the time
     * as LIST printed it, and asks the server only for the other links.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::ListDirectory>>
    runOperation(MetadataSession session, RemoteIo::ListDirectory operation);

    /**
     * @brief statPath(): the attributes of the end of a chain of symlinks.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::Stat>>
    runOperation(MetadataSession session, RemoteIo::Stat operation);

    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::Lstat>>
    runOperation(MetadataSession session, RemoteIo::Lstat operation);

    /**
     * @brief MKD, the permissions are the server's: FTP has no mode for a new directory.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::CreateNewDirectory>>
    runOperation(MetadataSession session, RemoteIo::CreateNewDirectory operation);

    /**
     * @brief MKD, and where something exists at the path, whether it is a directory or a link to one.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::CreateDirectoryIfItDoesntExist>>
    runOperation(MetadataSession session, RemoteIo::CreateDirectoryIfItDoesntExist operation);

    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::CreateEmptyFile>>
    runOperation(MetadataSession session, RemoteIo::CreateEmptyFile operation);

    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::RemoveFile>>
    runOperation(MetadataSession session, RemoteIo::RemoveFile operation);

    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::RemoveEmptyDirectory>>
    runOperation(MetadataSession session, RemoteIo::RemoveEmptyDirectory operation);

    /**
     * @brief removeEntry() for each path: DELE, RMD where the lookup behind its refusal found a directory.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::RemoveAll>>
    runOperation(MetadataSession session, RemoteIo::RemoveAll operation);

    /**
     * @brief Lists each directory, a path that cannot be listed is looked up: what is no directory is left out, what
     *        does not exist fails.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::FilterOutEmptyDirectories>>
    runOperation(MetadataSession session, RemoteIo::FilterOutEmptyDirectories operation);

    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::Rename>>
    runOperation(MetadataSession session, RemoteIo::Rename operation);

    /**
     * @brief FTP has no hard links: AlreadyExists when the destination exists, Unsupported when it is free.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::LinkIntoPlace>>
    runOperation(MetadataSession session, RemoteIo::LinkIntoPlace operation);

    /**
     * @brief RETR on the connection for commands, ABOR once maximumSize bytes came.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::ReadFile>>
    runOperation(MetadataSession session, RemoteIo::ReadFile operation);

    /**
     * @brief setModificationTime() with the time in whole seconds.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::SetModificationTime>>
    runOperation(MetadataSession session, RemoteIo::SetModificationTime operation);

    /**
     * @brief A directory as canonicalDirectory() resolves it. Anything else is its parent resolved that way with its
     *        name. A symlink leads on from there, so ".." in its content starts at the resolved parent, and is
     *        resolved as RemotePath::resolveLink() does. The lookup that tells a file from a link is the one that
     *        explains the refused CWD, see canonicalDirectoryOrEntry().
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::CanonicalPath>>
    runOperation(MetadataSession session, RemoteIo::CanonicalPath operation);

    /**
     * @brief Follows the link targets that LIST names, with lstatPath() at every step.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::ReadLinkDeep>>
    runOperation(MetadataSession session, RemoteIo::ReadLinkDeep operation);

    /**
     * @brief The link target that lstatPath() read from LIST.
     */
    boost::asio::awaitable<RemoteIo::OperationResult<RemoteIo::ReadLink>>
    runOperation(MetadataSession session, RemoteIo::ReadLink operation);
}
