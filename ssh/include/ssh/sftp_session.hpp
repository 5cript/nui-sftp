#pragma once

#include <libssh/libsshpp.hpp>
#include <libssh/sftp.h>
#include <remote_io/async/buffer_provider.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/async/processing_strand.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system.hpp>
#include <ssh/async/transfer_chunk_controller.hpp>
#include <ssh/file_information.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/session.hpp>
#include <paths/remote_path.hpp>
#include <utility/user_directories.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <future>
#include <optional>
#include <string_view>
#include <expected>
#include <filesystem>
#include <utility>
#include <type_traits>

#include <fcntl.h>

namespace SecureShell
{
    class Session;

    /**
     * @brief The SFTP channel of a session as a remote file system. Every operation completes when it starts, on the
     *        processing thread of the strand.
     */
    class SftpSession : public RemoteIo::RemoteFileSystemImplementation<SftpSession>
    {
      public:
        using Error = RemoteIo::RemoteError;
        /** @brief The libssh SFTP session, which sftp_free ends on the server too. */
        using NativeSession = std::unique_ptr<sftp_session_struct, decltype(&sftp_free)>;
        using DeepLinkResult = RemoteIo::DeepLinkResult;
        friend class FileStream;
        friend class RemoteIo::RemoteFileSystemImplementation<SftpSession>;

        SftpSession(Session* owner, std::unique_ptr<RemoteIo::ProcessingStrand> strand, NativeSession session);
        ~SftpSession() override;
        SftpSession(SftpSession const&) = delete;
        SftpSession& operator=(SftpSession const&) = delete;
        SftpSession(SftpSession&&) = delete;
        SftpSession& operator=(SftpSession&&) = delete;

        operator sftp_session()
        {
            return session_.get();
        }

        bool close() override;

        /**
         * @brief Closes the session and every stream of it.
         * @param isBackElement Whether this is the last SFTP session of its owner, which is then removed faster.
         * @return false when it was closed already.
         */
        bool close(bool isBackElement);

        /**
         * @brief Rename replaces an existing destination when the server has posix-rename@openssh.com, a plain SFTP
         *        rename refuses it.
         */
        RemoteIo::Capabilities capabilities() const override
        {
            return capabilities_;
        }

        /**
         * @brief Retrieves the last error that occurred. May contain success. A closed connection, channel or session
         *        is ConnectionLost, whatever the server answered before.
         */
        Error lastError() const;

        /**
         * @brief Lists the contents of a directory.
         *
         * @param path
         * @param resolveLinks Reads the literal and the target of every symlink, two more round trips each. Without
         *        it a symlink is listed as the link itself only.
         * @return std::future<std::expected<std::vector<FileInformation>, Error>>
         */
        std::future<std::expected<std::vector<FileInformation>, Error>>
        listDirectory(Paths::RemotePath const& path, bool resolveLinks = true);

        /**
         * @brief In-strand variant of listDirectory. Must be called from within the processing thread.
         *
         * @param path
         * @param resolveLinks See @ref listDirectory.
         * @return std::expected<std::vector<FileInformation>, Error>
         */
        std::expected<std::vector<FileInformation>, Error>
        listDirectoryInStrand(Paths::RemotePath const& path, bool resolveLinks = true);

        /**
         * @brief Create a directory.
         *
         * @param path
         * @param permissions
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> createDirectory(
            Paths::RemotePath const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_all
        );

        /**
         * @brief In-strand variant of createDirectory. Must be called from within the processing thread.
         */
        std::expected<void, Error> createDirectoryInStrand(
            Paths::RemotePath const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_all
        );

        /**
         * @brief Create a directory if it does not already exist.
         *
         * @param path
         * @param permissions
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> createDirectoryIfItDoesntExist(
            Paths::RemotePath const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_all
        );

        /**
         * @brief In-strand variant of createDirectoryIfItDoesntExist. Must be called from within the processing thread.
         */
        std::expected<void, Error> createDirectoryIfItDoesntExistInStrand(
            Paths::RemotePath const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_all
        );

        /**
         * @brief Opens a file, creating it if it does not exist, then closes it.
         *
         * @param path
         * @param permissions
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> createFile(
            Paths::RemotePath const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );

        /**
         * @brief In-strand variant of createFile. Must be called from within the processing thread.
         */
        std::expected<void, Error> createFileInStrand(
            Paths::RemotePath const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );

        /**
         * @brief Removes a file.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> removeFile(Paths::RemotePath const& path);

        /**
         * @brief In-strand variant of removeFile. Must be called from within the processing thread.
         */
        std::expected<void, Error> removeFileInStrand(Paths::RemotePath const& path);

        /**
         * @brief Removes a directory.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> removeDirectory(Paths::RemotePath const& path);

        /**
         * @brief In-strand variant of removeDirectory. Must be called from within the processing thread.
         */
        std::expected<void, Error> removeDirectoryInStrand(Paths::RemotePath const& path);

        /**
         * @brief Removes each of @p paths without recursing: a directory only when it is empty, anything else is
         *        unlinked. Stops at the first failure.
         */
        std::future<std::expected<void, Error>> removeAll(std::vector<Paths::RemotePath> paths);

        /**
         * @brief Checks a list of directories for non-emptiness.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<std::vector<Paths::RemotePath>, Error>>
        filterOutEmptyDirectories(std::vector<Paths::RemotePath> directories);

        /**
         * @brief Gets the attributes of a file or directory (resolves links).
         *
         * @param path
         * @return std::future<std::expected<FileInformation, Error>>
         */
        std::future<std::expected<FileInformation, Error>> stat(Paths::RemotePath const& path);

        /**
         * @brief In-strand variant of stat. Must be called from within the processing thread.
         */
        std::expected<FileInformation, Error> statInStrand(Paths::RemotePath const& path);

        /**
         * @brief Gets the attributes of a file or directory and if it is a link, the attributes of the link itself.
         *
         * @param path
         * @return std::future<std::expected<FileInformation, Error>>
         */
        std::future<std::expected<FileInformation, Error>> lstat(Paths::RemotePath const& path);

        /**
         * @brief In-strand variant of lstat. Must be called from within the processing thread.
         */
        std::expected<FileInformation, Error> lstatInStrand(Paths::RemotePath const& path);

        /**
         * @brief Sets the attributes of a file or directory.
         *
         * @param path
         * @param attributes
         * @return std::future<std::expected<FileInformation, Error>>
         */
        std::future<std::expected<void, Error>> stat(Paths::RemotePath const& path, sftp_attributes attributes);

        /**
         * @brief In-strand variant of setstat. Must be called from within the processing thread.
         */
        std::expected<void, Error> statInStrand(Paths::RemotePath const& path, sftp_attributes attributes);

        /**
         * @brief Sets the access and modification time of a path. Must be called from within the processing thread.
         * @return Unsupported for a time SFTP cannot carry, before 1970 or after 2106.
         */
        std::expected<void, Error> setModificationTimeInStrand(
            Paths::RemotePath const& path,
            std::chrono::system_clock::time_point modificationTime
        );

        /**
         * @brief Sets the owner of a file or directory.
         *
         * @param path
         * @param owner
         * @param group
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> chown(Paths::RemotePath const& path, uid_t owner, gid_t group);

        /**
         * @brief In-strand variant of chown. Must be called from within the processing thread.
         */
        std::expected<void, Error> chownInStrand(Paths::RemotePath const& path, uid_t owner, gid_t group);

        /**
         * @brief Sets the permissions of a file or directory.
         *
         * @param path
         * @param mode
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> chmod(Paths::RemotePath const& path, std::filesystem::perms perms);

        /**
         * @brief In-strand variant of chmod. Must be called from within the processing thread.
         */
        std::expected<void, Error> chmodInStrand(Paths::RemotePath const& path, std::filesystem::perms perms);

        /**
         * @brief Move a file or directory.
         */
        std::future<std::expected<void, Error>>
        rename(Paths::RemotePath const& source, Paths::RemotePath const& destination);

        /**
         * @brief In-strand variant of rename. Must be called from within the processing thread.
         *        Replaces an existing destination when the server supports posix-rename, use
         *        @ref renameNoReplaceInStrand or @ref renameReplaceInStrand to choose.
         */
        std::expected<void, Error>
        renameInStrand(Paths::RemotePath const& source, Paths::RemotePath const& destination);

        /**
         * @brief RemoteIo::renameNoReplaceInStrand on this session. A regular file moves atomically through
         *        hardlink@openssh.com where the server has it and the link succeeds.
         */
        std::expected<void, Error> renameNoReplaceInStrand(
            Paths::RemotePath const& source,
            Paths::RemotePath const& destination,
            std::optional<SharedData::FileType> sourceType = std::nullopt
        );

        /**
         * @brief RemoteIo::renameReplaceInStrand on this session, atomic with posix-rename@openssh.com.
         */
        std::expected<void, Error> renameReplaceInStrand(
            Paths::RemotePath const& source,
            Paths::RemotePath const& destination,
            std::string_view asideSuffix
        );

        /**
         * @brief RemoteIo::moveIntoPlaceInStrand on this session.
         */
        std::expected<void, Error> moveIntoPlaceInStrand(
            Paths::RemotePath const& source,
            Paths::RemotePath const& destination,
            bool replace,
            std::string_view asideSuffix,
            std::optional<SharedData::FileType> sourceType = std::nullopt
        );

        /**
         * @brief Whether the server announced the SFTP extension @p name with @p version.
         */
        bool supportsExtensionInStrand(char const* name, char const* version = "1") const;

        enum class OpenType : int
        {
            Read = O_RDONLY,
            Write = O_WRONLY,
            ReadWrite = O_RDWR,
            Create = O_CREAT,
            Truncate = O_TRUNC,
            Exclusive = O_EXCL,
        };

        std::future<std::expected<std::weak_ptr<FileStream>, Error>>
        openFile(Paths::RemotePath const& path, OpenType openType, std::filesystem::perms permissions);

        /**
         * @brief In-strand variant of openFile. Must be called from within the processing thread.
         */
        std::expected<std::weak_ptr<FileStream>, Error>
        openFileInStrand(Paths::RemotePath const& path, OpenType openType, std::filesystem::perms permissions);

        /**
         * @brief Determines the directory the server starts the user in, which is the home directory on conforming
         * servers. Falls back to /home/<user> when the server does not answer with an absolute path, or to / when that
         * does not exist either.
         *
         * @return std::future<std::expected<Paths::RemotePath, Error>>
         */
        std::future<std::expected<Paths::RemotePath, Error>> homeDirectory();

        /**
         * @brief In-strand variant of homeDirectory. Must be called from within the processing thread.
         */
        std::expected<Paths::RemotePath, Error> homeDirectoryInStrand();

        /**
         * @brief The directory the server starts the user in, resolved by the server (realpath of "." on OpenSSH),
         *        without any fallback: an error when the server does not answer with an absolute path. Must be
         *        called from within the processing thread.
         */
        std::expected<Paths::RemotePath, Error> canonicalHomeDirectoryInStrand();

        /**
         * @brief The server's canonical form of the existing @p path, with every symlink resolved (realpath on
         *        OpenSSH). Must be called from within the processing thread.
         */
        std::expected<Paths::RemotePath, Error> canonicalPathInStrand(Paths::RemotePath const& path);

        /**
         * @brief Reads the XDG user directories from <home>/.config/user-dirs.dirs. A missing file yields no
         * directories instead of an error.
         *
         * @param home The home directory of the user, see homeDirectory.
         * @return std::future<std::expected<std::vector<Utility::UserDirectory>, Error>>
         */
        std::future<std::expected<std::vector<Utility::UserDirectory>, Error>>
        userDirectories(Paths::RemotePath const& home);

        /**
         * @brief In-strand variant of userDirectories. Must be called from within the processing thread.
         */
        std::expected<std::vector<Utility::UserDirectory>, Error>
        userDirectoriesInStrand(Paths::RemotePath const& home);

        std::future<std::expected<sftp_limits_struct, Error>> limits();

        /**
         * @brief In-strand variant of limits. Must be called from within the processing thread.
         */
        std::expected<sftp_limits_struct, Error> limitsInStrand();

        RemoteIo::ProcessingStrand* strand() const override
        {
            return strand_.get();
        }

        /**
         * @brief The round trip of this session's link, which bounds how long a transfer's turn blocks.
         */
        TransferChunkController& chunkController() noexcept
        {
            return chunkController_;
        }

        TransferChunkController const& chunkController() const noexcept
        {
            return chunkController_;
        }

        /**
         * @brief Shared buffer pool for this session's file transfers.
         */
        RemoteIo::IBufferProvider& bufferProvider() noexcept override
        {
            return *bufferProvider_;
        }

        std::future<std::expected<DeepLinkResult, Error>>
        readLinkDeep(Paths::RemotePath const& path, int maxDepth = 10);

        /**
         * @brief In-strand variant of readLinkDeep. Must be called from within the processing thread.
         */
        std::expected<DeepLinkResult, Error> readLinkDeepInStrand(Paths::RemotePath const& path, int maxDepth = 10);

        /**
         * @brief The literal of the symlink @p path, valid UTF-8 like a listed link target (U+FFFD for every invalid
         *        byte). Must be called from within the processing thread.
         */
        std::expected<std::string, Error> readLinkInStrand(Paths::RemotePath const& path);

        /**
         * @brief Creates a symlink at @p linkPath with the content @p target, which may be relative.
         */
        std::future<std::expected<void, Error>>
        createSymLink(std::string const& target, Paths::RemotePath const& linkPath);

        /**
         * @brief In-strand variant of createSymLink. Must be called from within the processing thread.
         */
        std::expected<void, Error> createSymLinkInStrand(std::string const& target, Paths::RemotePath const& linkPath);

      protected:
        void dispatch(std::function<void()> start) override;

      private:
        template <typename OperationT>
        void startOperation(RemoteIo::Pending<OperationT> pending)
        {
            pending.complete(run(pending.operation));
        }

        /**
         * @brief inStrand without its completion, which SFTP never defers.
         */
        template <typename OperationT>
        RemoteIo::OperationResult<OperationT> runInStrand(OperationT const& operation)
        {
            assert(strand_->withinProcessingThread());
            if (RemoteIo::hasUnsetPath(operation))
                return std::unexpected(unsetPathError());
            return run(operation);
        }

        RemoteIo::OperationResult<RemoteIo::ListDirectory> run(RemoteIo::ListDirectory const& operation);
        RemoteIo::OperationResult<RemoteIo::Stat> run(RemoteIo::Stat const& operation);
        RemoteIo::OperationResult<RemoteIo::Lstat> run(RemoteIo::Lstat const& operation);
        RemoteIo::OperationResult<RemoteIo::CreateNewDirectory> run(RemoteIo::CreateNewDirectory const& operation);
        RemoteIo::OperationResult<RemoteIo::CreateDirectoryIfItDoesntExist>
        run(RemoteIo::CreateDirectoryIfItDoesntExist const& operation);
        RemoteIo::OperationResult<RemoteIo::CreateEmptyFile> run(RemoteIo::CreateEmptyFile const& operation);
        RemoteIo::OperationResult<RemoteIo::RemoveFile> run(RemoteIo::RemoveFile const& operation);
        RemoteIo::OperationResult<RemoteIo::RemoveEmptyDirectory> run(RemoteIo::RemoveEmptyDirectory const& operation);
        RemoteIo::OperationResult<RemoteIo::RemoveAll> run(RemoteIo::RemoveAll const& operation);
        RemoteIo::OperationResult<RemoteIo::FilterOutEmptyDirectories>
        run(RemoteIo::FilterOutEmptyDirectories const& operation);
        RemoteIo::OperationResult<RemoteIo::Rename> run(RemoteIo::Rename const& operation);
        RemoteIo::OperationResult<RemoteIo::LinkIntoPlace> run(RemoteIo::LinkIntoPlace const& operation);
        RemoteIo::OperationResult<RemoteIo::OpenFile> run(RemoteIo::OpenFile const& operation);
        RemoteIo::OperationResult<RemoteIo::SetModificationTime> run(RemoteIo::SetModificationTime const& operation);
        RemoteIo::OperationResult<RemoteIo::HomeDirectory> run(RemoteIo::HomeDirectory const& operation);
        RemoteIo::OperationResult<RemoteIo::CanonicalHomeDirectory>
        run(RemoteIo::CanonicalHomeDirectory const& operation);
        RemoteIo::OperationResult<RemoteIo::CanonicalPath> run(RemoteIo::CanonicalPath const& operation);
        RemoteIo::OperationResult<RemoteIo::ReadTransferLimits> run(RemoteIo::ReadTransferLimits const& operation);
        RemoteIo::OperationResult<RemoteIo::ReadLinkDeep> run(RemoteIo::ReadLinkDeep const& operation);
        RemoteIo::OperationResult<RemoteIo::ReadLink> run(RemoteIo::ReadLink const& operation);
        RemoteIo::OperationResult<RemoteIo::CreateSymLink> run(RemoteIo::CreateSymLink const& operation);

        /**
         * @brief Opens @p path with the sftp_open @p flags and keeps the stream until it is closed.
         */
        std::expected<std::shared_ptr<FileStream>, Error>
        openStreamInStrand(Paths::RemotePath const& path, int flags, std::filesystem::perms permissions);

        /**
         * @brief Asserts the call runs in the processing thread. Refuses a closed session, whose null libssh session
         *        most libssh calls dereference, and an unset path, which the server would take as the start directory.
         */
        template <typename... PathTs>
        std::expected<void, Error> requireSetInStrand(PathTs const&... paths) const
        {
            assert(strand_->withinProcessingThread());
            if (!session_)
                return std::unexpected(lastError());
            if ((paths.empty() || ...))
                return std::unexpected(unsetPathError());
            return {};
        }

        static Error unsetPathError();

        /**
         * @brief The absolute path the server canonicalizes @p path to.
         */
        std::expected<Paths::RemotePath, Error> canonicalizeInStrand(char const* path);

        static Error alreadyExists(Paths::RemotePath const& destination);

        void fileStreamRemoveItself(FileStream* stream, bool isBackElement);
        void removeAllFileStreams();

      private:
        Session* owner_;
        std::unique_ptr<RemoteIo::ProcessingStrand> strand_;
        /** @brief Freed by close on the processing thread, null afterwards. */
        NativeSession session_;
        std::shared_ptr<RemoteIo::IBufferProvider> bufferProvider_;
        std::vector<std::shared_ptr<FileStream>> fileStreams_;
        TransferChunkController chunkController_{};
        RemoteIo::Capabilities capabilities_;
    };

    constexpr inline auto operator|(SftpSession::OpenType a, SftpSession::OpenType b)
    {
        return static_cast<SftpSession::OpenType>(static_cast<int>(a) | static_cast<int>(b));
    }
}