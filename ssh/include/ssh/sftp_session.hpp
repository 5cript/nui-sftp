#pragma once

#include <libssh/libsshpp.hpp>
#include <libssh/sftp.h>
#include <ssh/async/buffer_provider.hpp>
#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>
#include <ssh/async/transfer_chunk_controller.hpp>
#include <ssh/file_information.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_error.hpp>
#include <ssh/session.hpp>
#include <paths/remote_path.hpp>
#include <utility/user_directories.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <future>
#include <expected>
#include <filesystem>
#include <utility>
#include <type_traits>

#include <fcntl.h>

namespace SecureShell
{
    class Session;

    class SftpSession : public std::enable_shared_from_this<SftpSession>
    {
      public:
        using Error = SftpError;
        friend class FileStream;

        SftpSession(Session* owner, std::unique_ptr<ProcessingStrand> strand, sftp_session session);
        ~SftpSession();
        SftpSession(SftpSession const&) = delete;
        SftpSession& operator=(SftpSession const&) = delete;
        SftpSession(SftpSession&&) = delete;
        SftpSession& operator=(SftpSession&&) = delete;

        operator sftp_session()
        {
            return session_;
        }

        bool close(bool isBackElement = false);

        template <typename FunctionT>
        void perform(FunctionT&& func)
        {
            strand_->pushTask(std::forward<FunctionT>(func));
        }

        template <typename FunctionT>
        auto performPromise(FunctionT&& func) -> std::future<std::invoke_result_t<std::decay_t<FunctionT>>>
        {
            return strand_->pushPromiseTask(std::forward<FunctionT>(func));
        }

        /**
         * @brief Retrieves the last error that occurred. May contain success.
         */
        SftpError lastError() const;

        /**
         * @brief Lists the contents of a directory.
         *
         * @param path
         * @return std::future<std::expected<std::vector<FileInformation>, Error>>
         */
        std::future<std::expected<std::vector<FileInformation>, Error>>
        listDirectory(Paths::RemotePath const& path);

        /**
         * @brief In-strand variant of listDirectory. Must be called from within the processing thread.
         *
         * @param path
         * @return std::expected<std::vector<FileInformation>, Error>
         */
        std::expected<std::vector<FileInformation>, Error> listDirectoryInStrand(Paths::RemotePath const& path);

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
         * @brief Removes everything in the provided paths.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
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
         * @brief Renames without ever replacing @p destination. Fails with SSH_FX_FILE_ALREADY_EXISTS when it
         *        exists. Regular files move atomically through hardlink@openssh.com where the server has it.
         */
        std::expected<void, Error>
        renameNoReplaceInStrand(Paths::RemotePath const& source, Paths::RemotePath const& destination);

        /**
         * @brief Renames over an existing @p destination without a moment where it is missing: atomically with
         *        posix-rename@openssh.com, otherwise by moving the destination aside to @p asideSuffix and
         *        back if the rename fails.
         */
        std::expected<void, Error> renameReplaceInStrand(
            Paths::RemotePath const& source,
            Paths::RemotePath const& destination,
            std::string_view asideSuffix
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

        ProcessingStrand* strand() const
        {
            return strand_.get();
        }

        /**
         * @brief Sizes this session's blocking data calls from measured round trips.
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
        IBufferProvider& bufferProvider() noexcept
        {
            return *bufferProvider_;
        }

        struct DeepLinkResult
        {
            /** @brief The content of the link at the given path, as stored on the server. */
            std::string linkContent;
            /** @brief The end of the link chain. */
            Paths::RemotePath linkTarget;
            /** @brief The attributes of @ref linkTarget, empty when it does not exist. */
            std::optional<FileInformation> targetInfo;
        };
        std::future<std::expected<DeepLinkResult, Error>>
        readLinkDeep(Paths::RemotePath const& path, int maxDepth = 10);

        /**
         * @brief In-strand variant of readLinkDeep. Must be called from within the processing thread.
         */
        std::expected<DeepLinkResult, Error> readLinkDeepInStrand(Paths::RemotePath const& path, int maxDepth = 10);

        /**
         * @brief Creates a symlink at @p linkPath with the content @p target, which may be relative.
         */
        std::future<std::expected<void, Error>>
        createSymLink(std::string const& target, Paths::RemotePath const& linkPath);

        /**
         * @brief In-strand variant of createSymLink. Must be called from within the processing thread.
         */
        std::expected<void, Error> createSymLinkInStrand(std::string const& target, Paths::RemotePath const& linkPath);

      private:
        /**
         * @brief Refuses an unset path before it reaches the server.
         */
        static std::expected<void, Error> requireSet(Paths::RemotePath const& path);

        /**
         * @brief Reads a file into a string, stopping after maximumSize bytes.
         */
        std::expected<std::string, Error>
        readSmallFileInStrand(Paths::RemotePath const& path, std::size_t maximumSize);

        void fileStreamRemoveItself(FileStream* stream, bool isBackElement);
        void removeAllFileStreams();

      private:
        Session* owner_;
        std::unique_ptr<ProcessingStrand> strand_;
        sftp_session session_;
        std::shared_ptr<IBufferProvider> bufferProvider_;
        std::vector<std::shared_ptr<FileStream>> fileStreams_;
        TransferChunkController chunkController_{};
    };

    constexpr inline auto operator|(SftpSession::OpenType a, SftpSession::OpenType b)
    {
        return static_cast<SftpSession::OpenType>(static_cast<int>(a) | static_cast<int>(b));
    }
}