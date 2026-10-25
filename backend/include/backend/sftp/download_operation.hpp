#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/async/buffer_provider.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/utility/move_detector.hpp>
#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <persistence/state/sftp_options.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <utility/link_safe_remove.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

class DownloadOperation : public Operation
{
  public:
    struct DownloadOperationOptions
    {
        std::function<void(
            std::uint64_t min,
            std::uint64_t max,
            std::uint64_t current,
            std::make_signed_t<std::size_t> bytesPerSecond
        )>
            progressCallback = [](auto, auto, auto, auto) {};
        Paths::RemotePath remotePath{};
        Paths::LocalPath localPath{};
        std::string tempFileSuffix{".filepart"};
        bool mayOverwrite{false};
        bool reserveSpace{false};
        bool tryContinue{Persistence::defaultTryContinue};
        bool inheritPermissions{Persistence::defaultInheritPermissions};
        bool doCleanup{true};
        bool bigFileOptimized{false};
        std::optional<std::filesystem::perms> filePermissions{std::nullopt};
        std::optional<std::filesystem::perms> directoryPermissions{std::nullopt};
        std::chrono::seconds futureTimeout{5};
        Persistence::SymlinkHandling symlinkHandling{Persistence::SymlinkHandling::AsSymlink};
        std::optional<SharedData::DirectoryEntry> entry{std::nullopt};
        /** @brief Create every missing parent directory of @ref localPath before opening the
         *         local file.  Off by default — enable for sync flows whose target subtree
         *         may not yet exist locally.
         */
        bool createMissingDirectories{false};
        /** @brief Mark the file as downloaded from the internet, see Utility::markAsDownloaded. */
        bool markAsDownloaded{Persistence::defaultMarkAsDownloaded};
        /**
         * @brief When set, a local path running through a symlink or junction below this directory is refused, so
         *        a link planted inside a downloaded tree cannot redirect the download out of it.
         */
        Paths::LocalPath linkFreeRoot{};
        /** @brief Directories below @ref linkFreeRoot already found to be no links, shared by a bulk download. */
        std::shared_ptr<Utility::VerifiedDirectories> verifiedDirectories{};
        /**
         * @brief What a sync compare saw at the destination. The transfer then fails with ChangedSinceCompare
         *        instead of replacing a destination that no longer holds it. A source that changed since is still
         *        transferred as it is now. Unset outside of sync.
         */
        std::optional<SharedData::ComparedState> comparedDestination{std::nullopt};
    };

    SecureShell::ProcessingStrand* strand() const override
    {
        if (auto stream = fileStream_.lock(); stream)
            return stream->strand();
        return strandOf(sftp_);
    }

    DownloadOperation(std::weak_ptr<SecureShell::SftpSession> sftp, DownloadOperationOptions options);
    ~DownloadOperation() override;
    DownloadOperation(DownloadOperation const&) = delete;
    DownloadOperation(DownloadOperation&&) = delete;
    DownloadOperation& operator=(DownloadOperation const&) = delete;
    DownloadOperation& operator=(DownloadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;
    std::expected<WorkStatus, Error> workInStrand() override;

    bool isBarrier() const noexcept override
    {
        return false;
    }

    /**
     * @brief How much parallel work does this operation do.
     *
     * @param parallel Maximum parallelism allowed.
     * @return The amount of parallel work that can be done maxed by parallel parameter.
     */
    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    SharedData::OperationType type() const override
    {
        return SharedData::OperationType::Download;
    }

    Paths::RemotePath const& remotePath() const
    {
        return options_.remotePath;
    }

    Paths::LocalPath const& localPath() const
    {
        return options_.localPath;
    }

    std::uint64_t totalSize() const
    {
        if (options_.entry)
            return options_.entry->size;
        return 0;
    }

    std::expected<void, DownloadOperation::Error> cancel(bool adoptCancelState) override;

    void pause(bool doPause) override;

    std::expected<void, Error> prepare();
    std::expected<void, Error> finalize();
    std::expected<void, Error> finalizeInStrand();

  private:
    /// Returns true if there is more data to read, false if the operation is complete.
    std::expected<bool, Error> readOnce();
    std::expected<bool, Error> readOnceInStrand();

    std::expected<void, Error> handleSymlink();
    std::expected<void, Error> handleSymlinkInStrand(SecureShell::SftpSession& sftp);

    bool commitBufferToFile(SecureShell::IFileStream::SignedSizeType bytesRead);

    /// Runs the remote-heavy portion of prepare() under a single strand umbrella.
    std::expected<void, Error> prepareInStrand(SecureShell::SftpSession& sftp);

    std::expected<void, Error> openOrAdoptFileInStrand(SecureShell::IFileStream& stream);

    /**
     * @brief Refuses @p path when it runs through a symlink or junction, below the link free root when one is set,
     *        otherwise only at @p path itself.
     */
    std::expected<void, Error> refuseLinkedPath(std::filesystem::path const& path) const;

    std::expected<SecureShell::SftpSession::DeepLinkResult, Error>
    readSymlinkInStrand(SecureShell::SftpSession& sftp, Paths::RemotePath const& remoteFullPath);

    /**
     * @brief The error of a file that ended before its listed size arrived.
     */
    Error sizeMismatch() const;

    /**
     * @brief Creates the temp file, or limits an existing one, to its owner until the final permissions are set.
     */
    std::expected<void, Error> restrictTempFile() const;

    /**
     * @brief Restricts the temp file and opens it empty for writing.
     */
    std::expected<void, Error> openEmptyTempFile();

    /**
     * @brief The permissions of the finished file: inherited, configured, or those of a new file.
     */
    std::expected<std::filesystem::perms, Error> finalPermissionsInStrand();

    /**
     * @brief Gives @p path the modification time of the remote file. A failure is only logged.
     */
    void applyModificationTime(std::filesystem::path const& path) const;

    void cleanup();

  private:
    std::weak_ptr<SecureShell::SftpSession> sftp_;
    std::weak_ptr<SecureShell::IFileStream> fileStream_;
    DownloadOperationOptions options_;
    /** @brief options_.remotePath, unset when none was given. */
    Paths::RemotePath remotePath_{};
    /** @brief options_.localPath for the filesystem calls, empty when none was given. */
    std::filesystem::path localNativePath_{};
    /** @brief Where the download is written before it replaces localNativePath_, empty when no local path was given. */
    std::filesystem::path tempNativePath_{};
    std::ofstream localFile_;
    SecureShell::BufferLease buffer_;
    std::shared_ptr<SecureShell::AsyncTransferContext> asyncTransferContext_;
    /** @brief Bytes already present in an adopted partial file. */
    std::uint64_t resumeOffset_{0u};
    /** @brief Bytes of the content in the temp file so far, an adopted partial file included. */
    std::uint64_t bytesInFile_{0u};
    /** @brief An adopted partial file already holds the whole content. */
    bool transferComplete_{false};
};