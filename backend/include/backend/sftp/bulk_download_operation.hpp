#pragma once

#include <backend/sftp/bulk_transfer_operation.hpp>
#include <backend/sftp/download_operation.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

class BulkDownloadOperation : public BulkTransferOperation
{
  public:
    struct BulkDownloadOperationOptions
    {
        BulkProgressCallback overallProgressCallback = [](SharedData::BulkProgress const&) {};
        /** @brief The scanned remote directory, only a label for a flat list. */
        Paths::RemotePath remotePath{};
        /** @brief Where the scanned directory is created, only a label for a flat list. */
        Paths::LocalPath localPath{};
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

    BulkDownloadOperation(std::weak_ptr<SecureShell::SftpSession> sftp, BulkDownloadOperationOptions options);
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
        Paths::RemotePath source;
        Paths::LocalPath destination;
        std::uint64_t sizeBytes;
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
        bool mayOverwrite{false};
        /** @brief A directory created without its content, with the link check of every local directory. */
        bool isDirectory{false};
        /** @brief See SharedData::BulkDownloadEntry::comparedDestination. */
        std::optional<SharedData::ComparedState> comparedDestination{};
    };
    void setPrescannedFileList(std::vector<PrescannedFile> files);

    /**
     * @brief The request that resumes this bulk on another connection: the entries it has not finished, as a flat
     *        list below its roots, with the options of @p request. None while the entries are not known yet, the
     *        backup then stays as it is. Also none when a remote symlink is among them, which a flat list could only
     * follow.
     */
    std::optional<SharedData::BulkDownloadRequest> resumeRequest(SharedData::BulkDownloadRequest request) const;

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
    bool isSkippableDirectoryError(Error const& error) const override;
    std::expected<void, Error> finishInStrand() override;

    bool sourceIsRemote() const noexcept override
    {
        return true;
    }

    std::string_view logName() const noexcept override
    {
        return "BulkDownloadOperation";
    }

  private:
    std::expected<void, Error>
    createLocalDirectory(Paths::LocalPath const& path, SharedData::DirectoryEntry const& entry);
    /**
     * @brief Remembers the permissions @p path gets once the bulk is done, so a directory without write
     *        permission still receives its content.
     */
    void deferDirectoryPermissions(std::filesystem::path const& path, SharedData::DirectoryEntry const& entryToInheritFrom);

  private:
    DownloadOperation::DownloadOperationOptions individualOptions_;
    /** @brief Created directories with the permissions they get at the end, parents before their children. */
    std::vector<std::pair<std::filesystem::path, std::filesystem::perms>> deferredDirectoryPermissions_{};
};
