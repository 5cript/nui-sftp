#pragma once

#include <backend/sftp/bulk_transfer_operation.hpp>
#include <backend/sftp/upload_operation.hpp>

#include <chrono>
#include <filesystem>
#include <string>

class BulkUploadOperation : public BulkTransferOperation
{
  public:
    struct BulkUploadOperationOptions
    {
        BulkProgressCallback overallProgressCallback = [](SharedData::BulkProgress const&) {};
        std::filesystem::path remotePath{};
        std::filesystem::path localPath{};
        UploadOperation::UploadOperationOptions individualOptions = {};
        bool failFast{false};
        // Files transferred at once, bounded by the queue's slot budget.
        int concurrency{1};
        std::chrono::milliseconds progressEmitInterval{100};
    };

    BulkUploadOperation(SecureShell::SftpSession& sftp, BulkUploadOperationOptions options);
    ~BulkUploadOperation() override;
    BulkUploadOperation(BulkUploadOperation const&) = delete;
    BulkUploadOperation(BulkUploadOperation&&) = delete;
    BulkUploadOperation& operator=(BulkUploadOperation const&) = delete;
    BulkUploadOperation& operator=(BulkUploadOperation&&) = delete;

    SharedData::OperationType type() const override;

    /** @brief See BulkDownloadOperation::PrescannedFile. */
    struct PrescannedFile
    {
        std::filesystem::path localSrc;
        std::filesystem::path remoteDst;
        std::uint64_t sizeBytes;
    };
    void setPrescannedFileList(std::vector<PrescannedFile> files);

  protected:
    std::expected<void, Error> prepareRootInStrand(SharedData::DirectoryEntry const& root) override;
    std::expected<void, Error> createDirectoryInStrand(std::size_t entryIndex) override;
    std::unique_ptr<Operation>
    makeChild(std::size_t entryIndex, std::function<void(std::uint64_t, std::uint64_t)> onProgress) override;
    std::filesystem::path displayPath(std::size_t entryIndex) const override;
    bool isSkippableError(Error const& error) const override;

    std::string_view logName() const noexcept override
    {
        return "BulkUploadOperation";
    }

  private:
    std::filesystem::path localPathOf(std::size_t entryIndex) const;
    std::filesystem::path remotePathOf(std::size_t entryIndex) const;
    std::filesystem::perms determinePerms(SharedData::DirectoryEntry const& entry) const;
    std::expected<void, Error>
    createRemoteDirectoryInStrand(std::filesystem::path const& path, SharedData::DirectoryEntry const& entry);

  private:
    UploadOperation::UploadOperationOptions individualOptions_;
};
