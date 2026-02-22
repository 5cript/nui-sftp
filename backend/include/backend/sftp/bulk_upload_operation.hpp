#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/file_stream.hpp>
#include <nui/utility/move_detector.hpp>
#include <backend/sftp/upload_operation.hpp>

#include <filesystem>
#include <expected>
#include <vector>
#include <memory>
#include <chrono>
#include <cstdint>

// TODO: concurrent uploads!
class BulkUploadOperation : public Operation
{
  public:
    struct BulkUploadOperationOptions
    {
        std::function<void(
            std::filesystem::path const& currentFile,
            std::uint64_t fileCurrentIndex,
            std::uint64_t fileCount,
            std::uint64_t currentFileBytes,
            std::uint64_t currentFileTotalBytes,
            std::uint64_t bytesCurrent,
            std::uint64_t bytesTotal,
            std::make_signed_t<std::size_t> bytesPerSecond
        )>
            overallProgressCallback = [](auto const&, auto, auto, auto, auto, auto, auto, auto) {};

        std::filesystem::path remotePath{};
        std::filesystem::path localPath{};
        UploadOperation::UploadOperationOptions individualOptions = {};
        bool failFast{false};
    };

    BulkUploadOperation(SecureShell::SftpSession& sftp, BulkUploadOperationOptions options);
    ~BulkUploadOperation() override;
    BulkUploadOperation(BulkUploadOperation const&) = delete;
    BulkUploadOperation(BulkUploadOperation&&) = delete;
    BulkUploadOperation& operator=(BulkUploadOperation const&) = delete;
    BulkUploadOperation& operator=(BulkUploadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;
    SharedData::OperationType type() const override;
    std::expected<void, Error> cancel(bool adoptCancelState) override;

    void setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t totalBytes);

    bool isBarrier() const noexcept override
    {
        return false;
    }

    // TODO: can do more than 1.
    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    SecureShell::ProcessingStrand* strand() const override;

    std::vector<std::pair<std::filesystem::path, Error>> getFailed() const;

  private:
    std::expected<WorkStatus, Error> workNormal();
    std::expected<WorkStatus, Error> workCurrentFile();
    std::filesystem::path fullLocalPath(SharedData::DirectoryEntry const& entry) const;
    std::filesystem::path fullRemotePath(SharedData::DirectoryEntry const& entry) const;
    std::filesystem::perms determinePerms(SharedData::DirectoryEntry const& entry) const;
    std::expected<void, Error>
    createDirectory(std::filesystem::path const& path, SharedData::DirectoryEntry const& entry);
    void completeCurrentUpload();

  private:
    SecureShell::SftpSession* sftp_;
    BulkUploadOperationOptions options_;
    std::unique_ptr<UploadOperation> currentUpload_;
    std::vector<SharedData::DirectoryEntry> entries_;
    std::vector<std::pair<std::filesystem::path, Error>> failedEntries_{};
    std::uint64_t totalBytes_{0};
    std::uint64_t currentIndex_{0};
    std::uint64_t currentBytes_{0};
    std::chrono::seconds futureTimeout_{5};
};