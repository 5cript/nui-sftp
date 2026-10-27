#pragma once

#include <backend/sftp/bulk_transfer_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
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

    BulkUploadOperation(std::weak_ptr<SecureShell::SftpSession> sftp, BulkUploadOperationOptions options);
    ~BulkUploadOperation() override;
    BulkUploadOperation(BulkUploadOperation const&) = delete;
    BulkUploadOperation(BulkUploadOperation&&) = delete;
    BulkUploadOperation& operator=(BulkUploadOperation const&) = delete;
    BulkUploadOperation& operator=(BulkUploadOperation&&) = delete;

    SharedData::OperationType type() const override;

    /**
     * @brief Completes without a transfer when the scanned directory is gone before the scan and the individual
     *        options let the source vanish, see UploadOperation::UploadOperationOptions::sourceMayVanish.
     */
    std::expected<WorkStatus, Error> work() override;

    /** @brief See BulkDownloadOperation::PrescannedFile. */
    struct PrescannedFile
    {
        Paths::LocalPath source;
        Paths::RemotePath destination;
        std::uint64_t sizeBytes;
        bool mayOverwrite{false};
        /** @brief A directory created without its content, its missing parents below the bulk's root with it. */
        bool isDirectory{false};
        std::optional<SharedData::ComparedState> comparedDestination{};
    };
    void setPrescannedFileList(std::vector<PrescannedFile> files);

    /**
     * @brief The request that resumes this bulk on another connection: the entries it has not finished, as a flat
     *        list below its roots, with the options of @p request. None while the entries are not known yet, the
     *        backup then stays as it is.
     */
    std::optional<SharedData::BulkUploadRequest> resumeRequest(SharedData::BulkUploadRequest request) const;

    void keepPartialFiles() override;

    /**
     * @brief The temporary files of the entries it has not finished, see unfinishedEntryIndices. Nothing when its
     *        files delete their partial file on cancel.
     */
    std::optional<KeptPartialFiles> keptPartialFiles() const override;

  protected:
    std::expected<void, Error>
    prepareRootInStrand(SecureShell::SftpSession& sftp, SharedData::DirectoryEntry const& root) override;
    std::expected<void, Error>
    createDirectoryInStrand(SecureShell::SftpSession& sftp, std::size_t entryIndex, EntryPaths const& paths) override;
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
    /**
     * @brief Creates the missing directories between the bulk's remote root and @p path, not @p path itself.
     */
    std::expected<void, Error>
    createMissingParentsInStrand(SecureShell::SftpSession& sftp, Paths::RemotePath const& path);
    std::expected<void, Error> createRemoteDirectoryInStrand(
        SecureShell::SftpSession& sftp,
        Paths::RemotePath const& path,
        SharedData::DirectoryEntry const& entry
    );

  private:
    UploadOperation::UploadOperationOptions individualOptions_;
};
