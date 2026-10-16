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
        /** @brief Where the scanned directory is created, only a label for a flat list. */
        Paths::RemotePath remotePath{};
        /** @brief The scanned local directory, only a label for a flat list. */
        Paths::LocalPath localPath{};
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
        Paths::LocalPath source;
        Paths::RemotePath destination;
        std::uint64_t sizeBytes;
        bool mayOverwrite{false};
    };
    void setPrescannedFileList(std::vector<PrescannedFile> files);

  protected:
    std::expected<void, Error> prepareRootInStrand(SharedData::DirectoryEntry const& root) override;
    std::expected<void, Error> createDirectoryInStrand(std::size_t entryIndex, EntryPaths const& paths) override;
    std::unique_ptr<Operation> makeChild(
        std::size_t entryIndex,
        EntryPaths const& paths,
        std::function<void(std::uint64_t, std::uint64_t)> onProgress
    ) override;
    bool isSkippableError(Error const& error) const override;

    bool sourceIsRemote() const noexcept override
    {
        return false;
    }

    std::string_view logName() const noexcept override
    {
        return "BulkUploadOperation";
    }

  private:
    std::filesystem::perms determinePerms(SharedData::DirectoryEntry const& entry) const;
    std::expected<void, Error>
    createRemoteDirectoryInStrand(Paths::RemotePath const& path, SharedData::DirectoryEntry const& entry);

  private:
    UploadOperation::UploadOperationOptions individualOptions_;
};
