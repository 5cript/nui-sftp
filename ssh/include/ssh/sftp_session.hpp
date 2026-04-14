#pragma once

#include <libssh/libsshpp.hpp>
#include <libssh/sftp.h>
#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>
#include <ssh/file_information.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_error.hpp>
#include <ssh/session.hpp>

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
        listDirectory(std::filesystem::path const& path);

        /**
         * @brief In-strand variant of listDirectory. Must be called from within the processing thread.
         *
         * @param path
         * @return std::expected<std::vector<FileInformation>, Error>
         */
        std::expected<std::vector<FileInformation>, Error> listDirectoryInStrand(std::filesystem::path const& path);

        /**
         * @brief Create a directory.
         *
         * @param path
         * @param permissions
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> createDirectory(
            std::filesystem::path const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_all
        );

        /**
         * @brief In-strand variant of createDirectory. Must be called from within the processing thread.
         */
        std::expected<void, Error> createDirectoryInStrand(
            std::filesystem::path const& path,
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
            std::filesystem::path const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_all
        );

        /**
         * @brief In-strand variant of createDirectoryIfItDoesntExist. Must be called from within the processing thread.
         */
        std::expected<void, Error> createDirectoryIfItDoesntExistInStrand(
            std::filesystem::path const& path,
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
            std::filesystem::path const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );

        /**
         * @brief In-strand variant of createFile. Must be called from within the processing thread.
         */
        std::expected<void, Error> createFileInStrand(
            std::filesystem::path const& path,
            std::filesystem::perms permissions = std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );

        /**
         * @brief Removes a file.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> removeFile(std::filesystem::path const& path);

        /**
         * @brief In-strand variant of removeFile. Must be called from within the processing thread.
         */
        std::expected<void, Error> removeFileInStrand(std::filesystem::path const& path);

        /**
         * @brief Removes a directory.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> removeDirectory(std::filesystem::path const& path);

        /**
         * @brief In-strand variant of removeDirectory. Must be called from within the processing thread.
         */
        std::expected<void, Error> removeDirectoryInStrand(std::filesystem::path const& path);

        /**
         * @brief Removes everything in the provided paths.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> removeAll(std::vector<std::filesystem::path> paths);

        /**
         * @brief Checks a list of directories for non-emptiness.
         *
         * @param path
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<std::vector<std::filesystem::path>, Error>>
        filterOutEmptyDirectories(std::vector<std::filesystem::path> directories);

        /**
         * @brief Gets the attributes of a file or directory (resolves links).
         *
         * @param path
         * @return std::future<std::expected<FileInformation, Error>>
         */
        std::future<std::expected<FileInformation, Error>> stat(std::filesystem::path const& path);

        /**
         * @brief In-strand variant of stat. Must be called from within the processing thread.
         */
        std::expected<FileInformation, Error> statInStrand(std::filesystem::path const& path);

        /**
         * @brief Gets the attributes of a file or directory and if it is a link, the attributes of the link itself.
         *
         * @param path
         * @return std::future<std::expected<FileInformation, Error>>
         */
        std::future<std::expected<FileInformation, Error>> lstat(std::filesystem::path const& path);

        /**
         * @brief In-strand variant of lstat. Must be called from within the processing thread.
         */
        std::expected<FileInformation, Error> lstatInStrand(std::filesystem::path const& path);

        /**
         * @brief Sets the attributes of a file or directory.
         *
         * @param path
         * @param attributes
         * @return std::future<std::expected<FileInformation, Error>>
         */
        std::future<std::expected<void, Error>> stat(std::filesystem::path const& path, sftp_attributes attributes);

        /**
         * @brief In-strand variant of setstat. Must be called from within the processing thread.
         */
        std::expected<void, Error> statInStrand(std::filesystem::path const& path, sftp_attributes attributes);

        /**
         * @brief Sets the owner of a file or directory.
         *
         * @param path
         * @param owner
         * @param group
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> chown(std::filesystem::path const& path, uid_t owner, gid_t group);

        /**
         * @brief In-strand variant of chown. Must be called from within the processing thread.
         */
        std::expected<void, Error> chownInStrand(std::filesystem::path const& path, uid_t owner, gid_t group);

        /**
         * @brief Sets the permissions of a file or directory.
         *
         * @param path
         * @param mode
         * @return std::future<std::expected<void, Error>>
         */
        std::future<std::expected<void, Error>> chmod(std::filesystem::path const& path, std::filesystem::perms perms);

        /**
         * @brief In-strand variant of chmod. Must be called from within the processing thread.
         */
        std::expected<void, Error> chmodInStrand(std::filesystem::path const& path, std::filesystem::perms perms);

        /**
         * @brief Move a file or directory.
         */
        std::future<std::expected<void, Error>>
        rename(std::filesystem::path const& source, std::filesystem::path const& destination);

        /**
         * @brief In-strand variant of rename. Must be called from within the processing thread.
         */
        std::expected<void, Error>
        renameInStrand(std::filesystem::path const& source, std::filesystem::path const& destination);

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
        openFile(std::filesystem::path const& path, OpenType openType, std::filesystem::perms permissions);

        /**
         * @brief In-strand variant of openFile. Must be called from within the processing thread.
         */
        std::expected<std::weak_ptr<FileStream>, Error>
        openFileInStrand(std::filesystem::path const& path, OpenType openType, std::filesystem::perms permissions);

        std::future<std::expected<sftp_limits_struct, Error>> limits();

        /**
         * @brief In-strand variant of limits. Must be called from within the processing thread.
         */
        std::expected<sftp_limits_struct, Error> limitsInStrand();

        ProcessingStrand* strand() const
        {
            return strand_.get();
        }

        struct DeepLinkResult
        {
            std::filesystem::path linkTarget;
            // target may not exist:
            std::optional<FileInformation> targetInfo;
        };
        std::future<std::expected<DeepLinkResult, Error>>
        readLinkDeep(std::filesystem::path const& path, int maxDepth = 10);

        /**
         * @brief In-strand variant of readLinkDeep. Must be called from within the processing thread.
         */
        std::expected<DeepLinkResult, Error>
        readLinkDeepInStrand(std::filesystem::path const& path, int maxDepth = 10);

        std::future<std::expected<void, Error>>
        createSymLink(std::filesystem::path const& target, std::filesystem::path const& linkPath);

        /**
         * @brief In-strand variant of createSymLink. Must be called from within the processing thread.
         */
        std::expected<void, Error>
        createSymLinkInStrand(std::filesystem::path const& target, std::filesystem::path const& linkPath);

      private:
        void fileStreamRemoveItself(FileStream* stream, bool isBackElement);
        void removeAllFileStreams();

      private:
        Session* owner_;
        std::unique_ptr<ProcessingStrand> strand_;
        sftp_session session_;
        std::vector<std::shared_ptr<FileStream>> fileStreams_;
    };

    constexpr inline auto operator|(SftpSession::OpenType a, SftpSession::OpenType b)
    {
        return static_cast<SftpSession::OpenType>(static_cast<int>(a) | static_cast<int>(b));
    }
}