#pragma once

#include <backend/sftp/operation.hpp>
#include <backend/sftp/unsafe_fast_mode.hpp>
#include <remote_io/async/buffer_provider.hpp>
#include <remote_io/remote_file_system.hpp>
#include <nui/utility/move_detector.hpp>
#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <persistence/state/sftp_options.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <utility/link_safe_remove.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
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
        bool doCleanup{Persistence::defaultDoCleanup};
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
        /** @brief See UnsafeFastMode, a link is always downloaded safely. */
        UnsafeFastMode unsafeFastMode{};
    };

    RemoteIo::ProcessingStrand* strand() const override
    {
        if (auto stream = fileStream_.lock(); stream)
            return stream->strand();
        return strandOf(fileSystem_);
    }

    DownloadOperation(std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem, DownloadOperationOptions options);
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

    bool mayOverwrite() const
    {
        return options_.mayOverwrite;
    }

    bool createsMissingDirectories() const
    {
        return options_.createMissingDirectories;
    }

    std::uint64_t totalSize() const
    {
        if (options_.entry)
            return options_.entry->size;
        return 0;
    }

    std::expected<void, DownloadOperation::Error> cancel(bool adoptCancelState) override;

    void pause(bool doPause) override;
    void keepPartialFiles() override;
    std::optional<KeptPartialFiles> keptPartialFiles() const override;

    std::expected<void, Error> prepare();
    std::expected<void, Error> finalize();
    std::expected<void, Error> finalizeInStrand();

  private:
    /// Returns true if there is more data to read, false if the operation is complete.
    std::expected<bool, Error> readOnce();
    std::expected<bool, Error> readOnceInStrand();

    std::expected<void, Error> handleSymlink();
    std::expected<void, Error> handleSymlinkInStrand(RemoteIo::IRemoteFileSystem& fileSystem);

    bool commitBufferToFile(RemoteIo::IRemoteFileStream::SignedSizeType bytesRead);

    /// Runs the remote-heavy portion of prepare() under a single strand umbrella.
    std::expected<void, Error> prepareInStrand(RemoteIo::IRemoteFileSystem& fileSystem);

    std::expected<void, Error> openOrAdoptFileInStrand(RemoteIo::IRemoteFileStream& stream);

    /**
     * @brief The unsafe fast mode's open: the destination itself, created or emptied. Only links in its path are
     *        refused, an existing destination that may not be overwritten was refused before.
     */
    std::expected<void, Error> openDestination();

    /**
     * @brief Leases the transfer buffer for chunks of at most @p lengthLimit bytes.
     */
    std::expected<void, Error> leaseBufferInStrand(RemoteIo::IRemoteFileSystem& fileSystem, std::size_t lengthLimit);

    /**
     * @brief The local file the download writes to: the destination in the unsafe fast mode, else the temp file.
     */
    std::filesystem::path const& writtenNativePath() const;

    /**
     * @brief Creates the parent directories of @p path when createMissingDirectories asks for it.
     */
    std::expected<void, Error> createMissingParents(std::filesystem::path const& path) const;

    /**
     * @brief Refuses @p path when it runs through a symlink or junction, below the link free root when one is set,
     *        otherwise only at @p path itself.
     */
    std::expected<void, Error> refuseLinkedPath(std::filesystem::path const& path) const;

    std::expected<RemoteIo::DeepLinkResult, Error>
    readSymlinkInStrand(RemoteIo::IRemoteFileSystem& fileSystem, Paths::RemotePath const& remoteFullPath);

    /**
     * @brief The error of a file that ended before its listed size arrived.
     */
    Error sizeMismatch() const;

    /**
     * @brief Creates the written file, or limits an existing one, to its owner until the final permissions are set.
     */
    std::expected<void, Error> restrictWrittenFile() const;

    /**
     * @brief Restricts the written file and opens it empty for writing.
     */
    std::expected<void, Error> openEmptyWrittenFile();

    /**
     * @brief What the server tells of the file at the end of the transfer, asked where the finished file inherits its
     *        permissions or where the entry's time is rounded. The unsafe fast mode asks for neither where the listing
     *        showed the permissions.
     * @return Nothing where nothing was asked, or where only the time was and the server did not tell it.
     */
    std::expected<std::optional<RemoteIo::FileInformation>, Error> remoteStateAtEndInStrand();

    /**
     * @brief The permissions of the finished file: inherited from @p atEnd or the entry, configured, or those of a new
     *        file.
     */
    std::filesystem::perms finalPermissions(std::optional<RemoteIo::FileInformation> const& atEnd) const;

    /**
     * @brief Gives @p path the modification time of the remote file: the one in @p atEnd where it is exact to the
     *        second, else the entry's. A failure is only logged.
     */
    void applyModificationTime(
        std::filesystem::path const& path,
        std::optional<RemoteIo::FileInformation> const& atEnd
    ) const;

    void cleanup();

  private:
    std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem_;
    std::weak_ptr<RemoteIo::IRemoteFileStream> fileStream_;
    DownloadOperationOptions options_;
    /** @brief options_.remotePath, unset when none was given. */
    Paths::RemotePath remotePath_{};
    /** @brief options_.localPath for the filesystem calls, empty when none was given. */
    std::filesystem::path localNativePath_{};
    /** @brief Where the download is written before it replaces localNativePath_, empty when no local path was given. */
    std::filesystem::path tempNativePath_{};
    std::ofstream localFile_;
    RemoteIo::BufferLease buffer_;
    std::shared_ptr<RemoteIo::AsyncTransferContext> asyncTransferContext_;
    /** @brief Bytes already present in an adopted partial file. */
    std::uint64_t resumeOffset_{0u};
    /** @brief Bytes of the content in the temp file so far, an adopted partial file included. */
    std::uint64_t bytesInFile_{0u};
    /** @brief An adopted partial file already holds the whole content. */
    bool transferComplete_{false};
    /** @brief The file is written in the unsafe fast mode, decided when it is opened. */
    bool unsafeFast_{false};
    /** @brief The unsafe fast mode opened the destination, which holds a partial file until the download completes. */
    bool openedDestination_{false};
};