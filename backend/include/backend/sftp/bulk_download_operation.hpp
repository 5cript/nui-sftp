#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/utility/move_detector.hpp>
#include <backend/sftp/download_operation.hpp>
#include <persistence/state/sftp_options.hpp>

#include <filesystem>
#include <string>

class BulkDownloadOperation : public Operation
{
  public:
    struct BulkDownloadOperationOptions
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
        DownloadOperation::DownloadOperationOptions individualOptions = {};
        bool asArchive{false};
        std::string archiveFormat{"tar"};
        std::string compressionMethod{"gz"};
        int compressionLevel{5};
        bool failFast{false};
    };

    BulkDownloadOperation(SecureShell::SftpSession& sftp, BulkDownloadOperationOptions options);
    ~BulkDownloadOperation() override;
    BulkDownloadOperation(BulkDownloadOperation const&) = delete;
    BulkDownloadOperation(BulkDownloadOperation&&) = delete;
    BulkDownloadOperation& operator=(BulkDownloadOperation const&) = delete;
    BulkDownloadOperation& operator=(BulkDownloadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;
    SharedData::OperationType type() const override;
    std::expected<void, Error> cancel(bool adoptCancelState) override;

    void setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t totalBytes);

    /**
     * @brief Prescanned-flat-list constructor path: the frontend already has
     *        per-file absolute source and destination paths plus known
     *        sizes, so the backend can skip scanning and build the entry
     *        list directly.
     *
     *        When this is used instead of setScanResult, workNormal's file
     *        branch consults the per-entry absolute-path override instead
     *        of computing paths from a shared root + tree structure.
     *        Directory creation (intermediate folders) is handled by
     *        DownloadOperation's createMissingDirectories path.
     *
     * @param files  Tuple (remoteAbsSrc, localAbsDst, sizeBytes) per file.
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

    bool isBarrier() const noexcept override
    {
        return false;
    }

    // TODO: can do more than 1.
    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    std::vector<std::pair<std::filesystem::path, Error>> getFailed() const;

    SecureShell::ProcessingStrand* strand() const override;

    // See ScanOperation for rationale — opt out of the queue's batched strand umbrella
    // until this op is converted to the *InStrand style.
    bool usesStrand() const noexcept override
    {
        return false;
    }

  private:
    std::expected<WorkStatus, Error> workNormal();
    std::expected<WorkStatus, Error> workAsArchive();
    std::expected<WorkStatus, Error> workCurrentFile();
    void completeCurrentDownload();
    std::filesystem::path fullLocalPath(SharedData::DirectoryEntry const& entry) const;
    std::expected<void, Error>
    applyPermsToDirectory(std::filesystem::path const& path, SharedData::DirectoryEntry const& entryToInheritFrom);

  private:
    SecureShell::SftpSession* sftp_;
    BulkDownloadOperationOptions options_;
    std::unique_ptr<DownloadOperation> currentDownload_;
    std::vector<SharedData::DirectoryEntry> entries_;
    std::vector<std::pair<std::filesystem::path, Error>> failedEntries_{};
    std::uint64_t totalBytes_{0};
    std::uint64_t currentIndex_{0};
    std::uint64_t currentBytes_{0};
    std::chrono::seconds futureTimeout_{5};
    // Prescanned-flat-list override: when non-empty, indexed by the same
    // currentIndex_ as entries_, and carries the absolute remote/local
    // paths verbatim so workNormal doesn't re-derive them from a shared
    // root.  Left empty when scanned via setScanResult.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> prescannedPathOverride_{};
};