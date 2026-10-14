#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/async/buffer_provider.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/utility/move_detector.hpp>
#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <persistence/state/sftp_options.hpp>

#include <filesystem>
#include <fstream>
#include <string>

class UploadOperation : public Operation
{
  public:
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
        bool tryContinue{false};
        bool inheritPermissions{false};
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

    std::expected<void, Error> openOrAdoptFileInStrand(SecureShell::SftpSession& sftp);

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

    std::filesystem::perms determinePerms(std::filesystem::perms localPerms) const;

    /**
     * @brief Gives the remote temp file the modification time of the local file. A failure is only logged.
     */
    void applyModificationTimeInStrand(SecureShell::SftpSession& sftp);

    void cleanup();

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
    std::ifstream localFile_;
    bool isSymlink_{false};
    std::size_t leftToUpload_{0};
    std::size_t totalSize_{0};
    SecureShell::BufferLease buffer_;
    std::shared_ptr<SecureShell::AsyncTransferContext> asyncTransferContext_;
};