#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/file_stream.hpp>
#include <nui/utility/move_detector.hpp>
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
        std::filesystem::path remotePath{};
        std::filesystem::path localPath{};
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

    UploadOperation(SecureShell::SftpSession& sftp, UploadOperationOptions options);
    ~UploadOperation() override;
    UploadOperation(UploadOperation const&) = delete;
    UploadOperation(UploadOperation&&) = delete;
    UploadOperation& operator=(UploadOperation const&) = delete;
    UploadOperation& operator=(UploadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;

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

    std::filesystem::path remotePath() const
    {
        return options_.remotePath;
    }

    std::filesystem::path localPath() const
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

    SecureShell::IFileStream::SignedSizeType commitFileToBuffer(SecureShell::IFileStream::SignedSizeType bytes);

    std::expected<void, Error> handleSymlink();

    std::expected<void, Error> openOrAdoptFile();

    /** @brief Ensures every ancestor directory of @p dir exists on the remote, creating
     *         any missing ones.  Idempotent: existing directories are left alone.
     *
     *  Walks up the chain via lstat and recurses into the grandparent before mkdir'ing
     *  the parent, so deeply-nested missing paths are created in one pass.
     */
    std::expected<void, Error> ensureRemoteDirectoryExists(std::filesystem::path const& dir);

    std::filesystem::perms determinePerms(std::filesystem::perms localPerms) const;

    void cleanup();

  private:
    SecureShell::SftpSession* sftp_;
    std::weak_ptr<SecureShell::IFileStream> fileStream_;
    UploadOperationOptions options_;
    std::ifstream localFile_;
    bool isSymlink_{false};
    std::size_t leftToUpload_{0};
    std::size_t totalSize_{0};
    std::array<char, 16384> buffer_{};
    std::shared_ptr<SecureShell::AsyncTransferContext> asyncTransferContext_;
};