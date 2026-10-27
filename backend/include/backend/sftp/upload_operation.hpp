#pragma once

#include <backend/sftp/operation.hpp>
#include <backend/sftp/unsafe_fast_mode.hpp>
#include <log/toast.hpp>
#include <ssh/async/buffer_provider.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/utility/move_detector.hpp>
#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <persistence/state/sftp_options.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <utility/shared_input_file_stream.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

class UploadOperation : public Operation
{
  public:
    /**
     * @brief The local state of a regular file an upload sent: it had it from before it was opened until the upload
     *        ended, so the server's copy holds the content it has in that state.
     */
    struct SentSource
    {
        std::uint64_t size{0};
        std::filesystem::file_time_type modified{};

        friend bool operator==(SentSource const&, SentSource const&) = default;
    };

    /**
     * @brief Hears of the regular file @p source once it replaced the destination, with the state it was sent in
     *        when it stayed unchanged (see SentSource). A continued upload and a file that changed meanwhile come
     *        without one. Called on the strand of the transfer.
     */
    using OnSent = std::function<void(Paths::LocalPath const& source, std::optional<SentSource> const& unchanged)>;

    /**
     * @brief What a cancelled upload does with the temporary file it wrote to.
     */
    enum class PartialFile
    {
        /** @brief It stays, a later upload of the same source continues it. */
        Kept,
        Discarded,
    };

    struct UploadOperationOptions
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
        bool tryContinue{Persistence::defaultTryContinue};
        bool inheritPermissions{Persistence::defaultInheritPermissions};
        /** @brief A canceled upload deletes its temporary file, see PartialFile. */
        bool doCleanup{Persistence::defaultDoCleanup};
        bool bigFileOptimized{false};
        std::optional<std::filesystem::perms> filePermissions{std::nullopt};
        std::optional<std::filesystem::perms> directoryPermissions{std::nullopt};
        std::chrono::seconds futureTimeout{5};
        Persistence::SymlinkHandling symlinkHandling{Persistence::SymlinkHandling::AsSymlink};
        /** @brief Create every missing parent directory of @ref remotePath before opening the
         *         file.  Adds one lstat (+ one mkdir per missing level) per upload, so it is
         *         off by default — enable for sync/priority flows that target freshly-diffed
         *         subtrees whose remote structure may not exist yet.
         */
        bool createMissingDirectories{false};
        /**
         * @brief What a sync compare saw at the destination. The transfer then fails with ChangedSinceCompare
         *        instead of replacing a destination that no longer holds it. A source that changed since is still
         *        transferred as it is now. Unset outside of sync.
         */
        std::optional<SharedData::ComparedState> comparedDestination{std::nullopt};
        /**
         * @brief A source that is gone when the upload starts is nothing to upload, it completes without a transfer.
         *        For uploads that mirror local files whose deletions are handled apart, see FileTracking::Tracker.
         */
        bool sourceMayVanish{false};
        /**
         * @brief Hears of a regular file the upload wrote to the server, see OnSent. A link is not told.
         */
        OnSent onSent{};
        /**
         * @brief The destination of a cancelled upload of the same source under its old name: its temporary file
         *        becomes this upload's before it is continued, see tryContinue. Unless this upload has one already.
         */
        std::optional<Paths::RemotePath> adoptPartialOf{};
        /**
         * @brief Whether the errors this upload logs when it fails are shown as toasts. Skipped for an owner that
         *        lists the failures itself, see FileTracking::Tracker.
         */
        Log::Toast failureToast{Log::Toast::Show};
        /** @brief See UnsafeFastMode, a link is always uploaded safely. */
        UnsafeFastMode unsafeFastMode{};
    };

    SecureShell::ProcessingStrand* strand() const override;

    UploadOperation(std::weak_ptr<SecureShell::SftpSession> sftp, UploadOperationOptions options);
    ~UploadOperation() override;
    UploadOperation(UploadOperation const&) = delete;
    UploadOperation(UploadOperation&&) = delete;
    UploadOperation& operator=(UploadOperation const&) = delete;
    UploadOperation& operator=(UploadOperation&&) = delete;

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
        return SharedData::OperationType::Upload;
    }

    Paths::RemotePath const& remotePath() const
    {
        return options_.remotePath;
    }

    Paths::LocalPath const& localPath() const
    {
        return options_.localPath;
    }

    std::size_t totalSize() const
    {
        return totalSize_;
    }

    std::expected<void, UploadOperation::Error> cancel(bool adoptCancelState) override;

    /**
     * @brief What a later cancel(true) does with the temporary file this upload wrote to, kept unless told otherwise.
     *        A temporary file it did not open itself always stays.
     */
    void partialFileOnCancel(PartialFile partial);

    void pause(bool doPause) override;

    std::expected<void, Error> prepare();
    std::expected<void, Error> finalize();

  private:
    /// Returns true if there is more data to write, false if the operation is complete.
    std::expected<bool, Error> writeOnce();
    std::expected<bool, Error> writeOnceInStrand();

    SecureShell::IFileStream::SignedSizeType commitFileToBuffer(SecureShell::IFileStream::SignedSizeType bytes);

    std::expected<void, Error> handleSymlink();
    std::expected<void, Error> handleSymlinkInStrand(SecureShell::SftpSession& sftp);

    /**
     * @brief Checks the local source and opens it, or for a link that is not followed only notes it is one: a
     *        regular file or a link, no other type, that exists.
     */
    std::expected<void, Error> openLocalSource();

    /**
     * @brief Whether the local source is not there at all, see UploadOperationOptions::sourceMayVanish. A source that
     *        cannot be checked is not gone, opening it fails the upload.
     */
    bool sourceIsGone() const;

    std::expected<void, Error> openOrAdoptFileInStrand(SecureShell::SftpSession& sftp);

    /**
     * @brief The unsafe fast mode's open: the destination itself, created, or truncated when it may be overwritten.
     *        Nothing is checked before, an existing destination that may not be overwritten fails the open.
     */
    std::expected<void, Error> openDestinationInStrand(SecureShell::SftpSession& sftp);

    /**
     * @brief Leases the transfer buffer for chunks of at most @p lengthLimit bytes.
     */
    std::expected<void, Error> leaseBufferInStrand(SecureShell::SftpSession& sftp, std::size_t lengthLimit);

    /**
     * @brief The remote file the upload writes to: the destination in the unsafe fast mode, else the temp file.
     */
    Paths::RemotePath const& writtenPath() const;

    /**
     * @brief Moves the temporary file of UploadOperationOptions::adoptPartialOf to tempPath_ when this upload has none
     *        there. A failure leaves the upload to start anew.
     */
    void takeOverPartialFileInStrand(SecureShell::SftpSession& sftp);

    /// Remote-heavy portion of prepare() — runs under a single strand umbrella.
    std::expected<void, Error> prepareInStrand(SecureShell::SftpSession& sftp);

    /// Remote-heavy portion of finalize() — runs under a single strand umbrella.
    std::expected<void, Error> finalizeInStrand(SecureShell::SftpSession& sftp);

    /**
     * @brief Whether the existing remote temp file of @p tempSize bytes ends like the same range of the source.
     */
    bool tempBelongsToSourceInStrand(SecureShell::SftpSession& sftp, std::uint64_t tempSize);

    /** @brief Ensures every ancestor directory of @p dir exists on the remote, creating
     *         any missing ones.  Idempotent: existing directories are left alone.
     *
     *  Walks up the chain via lstat and recurses into the grandparent before mkdir'ing
     *  the parent, so deeply-nested missing paths are created in one pass.
     *  Must be called from within the processing thread.
     */
    std::expected<void, Error>
    ensureRemoteDirectoryExistsInStrand(SecureShell::SftpSession& sftp, Paths::RemotePath const& dir);

    /**
     * @brief The mode the remote file is created with, always with owner write: the sanitized @p localPerms when
     *        inheriting (0444 when unknown), otherwise the configured file mode or 0660. The server's umask applies
     *        to it like to any new file.
     */
    std::filesystem::perms determinePerms(std::filesystem::perms localPerms) const;

    /**
     * @brief The mode the remote file is created with, see determinePerms, from the local permissions of the source.
     */
    std::filesystem::perms remoteFilePerms() const;

    /**
     * @brief Gives the remote file it writes to, see writtenPath, the modification time of the local file. A failure
     *        is only logged.
     */
    void applyModificationTimeInStrand(SecureShell::SftpSession& sftp);

    /**
     * @brief Tells UploadOperationOptions::onSent of the source once it replaced the destination, with its state
     *        unless it changed since it was opened.
     */
    void reportSent() const;

    void cleanup();

    /**
     * @brief What cancel() does, on the processing thread unless the session is gone: the transfer reads the local
     *        file and writes the temporary one there.
     */
    void cancelInStrand(bool adoptCancelState);

    /**
     * @brief Queues the removal of @p path on the strand of the transfer, after the file was closed on the server. A
     *        failure is only logged.
     */
    void removeRemoteFile(Paths::RemotePath const& path);

  private:
    std::weak_ptr<SecureShell::SftpSession> sftp_;
    std::weak_ptr<SecureShell::IFileStream> fileStream_;
    UploadOperationOptions options_;
    /** @brief options_.remotePath, unset when none was given. */
    Paths::RemotePath remotePath_{};
    /** @brief options_.localPath for the filesystem calls, empty when none was given. */
    std::filesystem::path localNativePath_{};
    /** @brief Where the upload is written before it replaces remotePath_. */
    Paths::RemotePath tempPath_{};
    /** @brief Lets others rename or delete the file while it uploads, which a std::ifstream refuses on Windows. */
    Utility::SharedInputFileStream localFile_;
    bool isSymlink_{false};
    /** @brief Why the transfer could not read the local file, the error its next work step fails with. */
    std::optional<Error> readFailure_{};
    /** @brief The state of the source right before it was opened, nothing when it could not be read. */
    std::optional<SentSource> sourceBeforeOpening_{};
    /** @brief The upload continued a temp file an earlier one left, whose content is not known to be the source's. */
    bool continued_{false};
    /** @brief The upload opened tempPath_ to write to it, created anew or continued. */
    bool openedTemporaryFile_{false};
    /** @brief The file is written in the unsafe fast mode, decided when it is opened. */
    bool unsafeFast_{false};
    /** @brief The unsafe fast mode opened the destination, which holds a partial file until the upload completes. */
    bool openedDestination_{false};
    PartialFile partialFileOnCancel_{PartialFile::Kept};
    std::size_t leftToUpload_{0};
    std::size_t totalSize_{0};
    SecureShell::BufferLease buffer_;
    std::shared_ptr<SecureShell::AsyncTransferContext> asyncTransferContext_;
};