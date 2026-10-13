#pragma once

#include <backend/sftp/operation.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include <backend/sftp/scan_phase.hpp>
#include <ssh/async/buffer_provider.hpp>
#include <ssh/async/transfer_context.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <tar_archive/archive.hpp>
#include <tar_archive/writer.hpp>
#include <tar_archive/entry_writer.hpp>
#include <tar_archive/compression.hpp>
#include <shared_data/directory_entry.hpp>
#include <tar_archive/member_header.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Packs a set of local files into a tar[.gz|.bz2|.zst|.xz] archive that
 *        is streamed straight into a remote SFTP file.
 *
 * Dual of ArchiveDownloadOperation: local files feed the tar writer, whose sink
 * writes into an SFTP FileStream. No temp file is staged — bytes go over the
 * wire as the archive is being built. SFTP's sftp_write() does not negotiate a
 * total size upfront, so this works without needing to know the compressed
 * output size ahead of time.
 *
 * V1 scope: only regular files. Directory entries in @p options.localPaths are
 * silently skipped (callers should flatten ahead of time).
 */
class ArchiveUploadOperation : public Operation
{
  public:
    struct Options
    {
        /** @brief Progress is reported as sum-of-local-file-sizes bytes transferred. */
        std::function<void(
            std::uint64_t min,
            std::uint64_t max,
            std::uint64_t current,
            std::make_signed_t<std::size_t> bytesPerSecond
        )>
            progressCallback = [](auto, auto, auto, auto) {};

        /** @brief Local filesystem paths to pack. Each is lstat'd in-strand on start. */
        std::vector<Paths::LocalPath> localPaths{};

        /** @brief Destination archive path on the remote side. */
        Paths::RemotePath remoteArchivePath{};

        /** @brief Compression codec; Auto means derive from remoteArchivePath extension. */
        TarArchive::Compression compression{TarArchive::Compression::Auto};

        /** @brief Codec-specific tuning. */
        TarArchive::CompressionOptions compressionOptions{};

        /** @brief Refuse to overwrite an existing remote file. */
        bool mayOverwrite{false};

        /** @brief The archive is written next to its destination under this suffix and renamed into place. */
        std::string tempFileSuffix{".filepart"};

        std::chrono::seconds futureTimeout{5};
    };

    ArchiveUploadOperation(SecureShell::SftpSession& sftp, Options options);
    ~ArchiveUploadOperation() override;
    ArchiveUploadOperation(ArchiveUploadOperation const&) = delete;
    ArchiveUploadOperation& operator=(ArchiveUploadOperation const&) = delete;
    ArchiveUploadOperation(ArchiveUploadOperation&&) = delete;
    ArchiveUploadOperation& operator=(ArchiveUploadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;
    std::expected<WorkStatus, Error> workInStrand() override;

    // The scan phase blocks on local filesystem calls, so it runs off the strand like a standalone scan.
    bool usesStrand() const noexcept override
    {
        return scanPhase_ == nullptr;
    }

    bool isBarrier() const noexcept override
    {
        return scanPhase_ != nullptr;
    }

    /**
     * @brief Scans the directory root of @p scan before packing; roots are scanned in the order added.
     */
    void addScanRoot(std::unique_ptr<LocalScanOperation> scan);

    bool scanning() const noexcept
    {
        return scanPhase_ != nullptr;
    }

    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    SharedData::OperationType type() const override
    {
        return SharedData::OperationType::ArchiveUpload;
    }

    SecureShell::ProcessingStrand* strand() const override
    {
        return sftp_ ? sftp_->strand() : nullptr;
    }

    std::expected<void, Error> cancel(bool adoptCancelState) override;

    /**
     * @brief Ingest a preceding LocalScanOperation's walk of @p localRootPath.
     *        Mirrors ArchiveDownloadOperation::setScanResultForRoot — the
     *        OperationQueue dispatcher calls this once per directory root
     *        before prepareInStrand runs, and flattenRoots consumes the stored
     *        entries instead of self-recursing so the scan work is visible as
     *        a separate queue card.
     *
     *        Safe to call multiple times (one per directory root in
     *        @ref Options::localPaths).  Matching is by the root's path as
     *        passed in the options — prepareInStrand looks up the same key.
     */
    void setScanResultForRoot(
        Paths::LocalPath const& localRootPath,
        std::vector<SharedData::DirectoryEntry> entries,
        std::uint64_t totalBytes
    );

  private:
    /**
     * @brief Archive-relative metadata paired with the full local source path.
     *        Directory entries have an unset source since they carry no
     *        payload.
     */
    struct ResolvedEntry
    {
        TarArchive::MemberHeader header{};
        Paths::LocalPath source{};
    };

    std::expected<void, Error> prepareInStrand();
    std::expected<void, Error> flattenRoots();
    /**
     * @brief Expand a preceding LocalScanOperation's flat entry vector into
     *        this op's ResolvedEntry list, prefixing each entry's tar path
     *        with @p archivePrefix and composing local full paths under
     *        @p localRootFullPath. Index 0 (the synthetic walker root) is
     *        skipped since its tar header is emitted by flattenRoots.
     */
    void appendPrescannedFromScan(
        std::vector<SharedData::DirectoryEntry> const& entries,
        Paths::LocalPath const& localRoot,
        Paths::RelativePath const& archivePrefix
    );
    std::expected<void, Error> openNextEntryInStrand();
    std::expected<bool, Error> readChunkInStrand();
    std::expected<void, Error> finaliseEntryInStrand();
    std::expected<void, Error> finaliseArchiveInStrand();
    bool isPayloadEntry(ResolvedEntry const& entry) const noexcept;

    void cleanup();

  private:
    SecureShell::SftpSession* sftp_;
    Options options_;
    std::vector<ResolvedEntry> resolvedEntries_{};
    std::optional<TarArchive::Writer> writer_{std::nullopt};
    std::optional<TarArchive::EntryWriter> currentEntry_{std::nullopt};
    std::ifstream currentLocalFile_{};
    std::weak_ptr<SecureShell::IFileStream> remoteStream_{};
    std::size_t currentEntryIndex_{0u};
    std::uint64_t currentEntryBytesRead_{0u};
    std::uint64_t totalBytesTransferred_{0u};
    std::uint64_t totalPayloadBytes_{0u};
    SecureShell::BufferLease buffer_;
    /**
     * @brief Pre-scanned entry lists delivered by preceding
     *        LocalScanOperation(s), keyed by the directory root's local full
     *        path. flattenRoots looks each directory root up here and errors
     *        out if none is present — there is no self-contained recurse.
     */
    std::map<Paths::LocalPath, std::pair<std::vector<SharedData::DirectoryEntry>, std::uint64_t>>
        prescannedRoots_{};
    /**
     * @brief Reused purely for its bytes-transferred + B/s meter machinery;
     *        the async pause/cancel side is unused because archive upload
     *        drives writes via writeInStrand rather than the FileStream async
     *        path that normally owns this context.
     */
    std::shared_ptr<SecureShell::AsyncTransferContext> throughputMeter_{
        std::make_shared<SecureShell::AsyncTransferContext>()};
    std::unique_ptr<ScanPhase<LocalScanOperation>> scanPhase_{};
    /** @brief options_.remoteArchivePath, unset when none was given. */
    Paths::RemotePath remoteArchivePath_{};
    /** @brief Where the archive is written before it replaces the destination. */
    Paths::RemotePath tempPath_{};
    /** @brief The temp file exists on the server and must go if the upload does not complete. */
    bool tempCreated_{false};
};
