#pragma once

#include <backend/sftp/bulk_transfer_operation.hpp>
#include <backend/sftp/download_operation.hpp>

#include <chrono>
#include <filesystem>
#include <string>

class BulkDownloadOperation : public BulkTransferOperation
{
  public:
    struct BulkDownloadOperationOptions
    {
        BulkProgressCallback overallProgressCallback = [](SharedData::BulkProgress const&) {};
        std::filesystem::path remotePath{};
        std::filesystem::path localPath{};
        DownloadOperation::DownloadOperationOptions individualOptions = {};
        bool asArchive{false};
        std::string archiveFormat{"tar"};
        std::string compressionMethod{"gz"};
        int compressionLevel{5};
        bool failFast{false};
        // Files transferred at once, bounded by the queue's slot budget.
        int concurrency{1};
        std::chrono::milliseconds progressEmitInterval{100};
    };

    BulkDownloadOperation(SecureShell::SftpSession& sftp, BulkDownloadOperationOptions options);
    ~BulkDownloadOperation() override;
    BulkDownloadOperation(BulkDownloadOperation const&) = delete;
    BulkDownloadOperation(BulkDownloadOperation&&) = delete;
    BulkDownloadOperation& operator=(BulkDownloadOperation const&) = delete;
    BulkDownloadOperation& operator=(BulkDownloadOperation&&) = delete;

    SharedData::OperationType type() const override;

    /**
     * @brief Prescanned-flat-list path: the frontend already has per-file absolute source and
     *        destination paths plus known sizes, so the backend can skip scanning.
     *        Intermediate directories are created by each DownloadOperation via
     *        createMissingDirectories.
     */
    struct PrescannedFile
    {
        std::filesystem::path remoteSrc;
        std::filesystem::path localDst;
        std::uint64_t sizeBytes;
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
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
        return "BulkDownloadOperation";
    }

  private:
    std::filesystem::path localPathOf(std::size_t entryIndex) const;
    std::expected<void, Error>
    applyPermsToDirectory(std::filesystem::path const& path, SharedData::DirectoryEntry const& entryToInheritFrom);

  private:
    DownloadOperation::DownloadOperationOptions individualOptions_;
};
