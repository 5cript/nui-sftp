#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/async/buffer_provider.hpp>
#include <ssh/async/transfer_context.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <tar_archive/archive.hpp>
#include <tar_archive/writer.hpp>
#include <tar_archive/entry_writer.hpp>
#include <tar_archive/compression.hpp>
#include <shared_data/directory_entry.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Packs a set of remote files into a single tar[.gz|.bz2|.zst|.xz] archive
 *        written to a local path.
 *
 * Each entry is streamed: we open the remote SFTP file, read it in fixed-size
 * chunks on the SFTP strand, and feed each chunk to a TarArchive::EntryWriter
 * whose underlying sink writes to a local std::ofstream (plus a compression layer
 * when requested). Nothing is buffered whole — the largest resident buffer is the
 * per-chunk 64 KiB scratch buffer.
 *
 * V1 scope: only regular files. Directory entries in @p options.entries are
 * silently skipped; callers should flatten directories ahead of time.
 */
class ArchiveDownloadOperation : public Operation
{
  public:
    struct Options
    {
        /**
         * @brief Progress callback. @p current / @p max is the byte-count across the
         *        *entire* archive (sum of all entry payloads), so progress bars
         *        advance smoothly over multi-file archives.
         */
        std::function<void(
            std::uint64_t min,
            std::uint64_t max,
            std::uint64_t current,
            std::make_signed_t<std::size_t> bytesPerSecond
        )>
            progressCallback = [](auto, auto, auto, auto) {};

        /**
         * @brief Roots to archive. Each entry represents one item the user selected:
         *
         *  - If type == Regular, the file is added under its basename.
         *  - If type == Directory, a directory entry is added plus every
         *    recursively-listed file/subdirectory under it. Archive-relative
         *    paths are rooted at the selection's basename so e.g. selecting
         *    `/remote/foo/` produces tar entries `foo/`, `foo/bar.txt`,
         *    `foo/sub/`, `foo/sub/baz.txt`.
         *  - Other types (symlinks, devices, ...) are silently skipped.
         *
         * `fullPath` must carry the full remote path; `path` is used only if
         * `fullPath` is empty.
         */
        std::vector<SharedData::DirectoryEntry> entries{};

        /** @brief Destination archive path on the local filesystem. */
        std::filesystem::path localArchivePath{};

        /** @brief Compression codec. Use TarArchive::Compression::Auto to pick from the extension. */
        TarArchive::Compression compression{TarArchive::Compression::Auto};

        /** @brief Codec-specific tuning. */
        TarArchive::CompressionOptions compressionOptions{};

        /** @brief Archive suffix used during the transfer; rename-on-finalise. */
        std::string tempFileSuffix{".filepart"};

        /** @brief Refuse to overwrite a pre-existing final archive. */
        bool mayOverwrite{false};

        /** @brief Create any missing parents of @ref localArchivePath. */
        bool createMissingDirectories{false};

        std::chrono::seconds futureTimeout{5};
    };

    ArchiveDownloadOperation(SecureShell::SftpSession& sftp, Options options);
    ~ArchiveDownloadOperation() override;
    ArchiveDownloadOperation(ArchiveDownloadOperation const&) = delete;
    ArchiveDownloadOperation& operator=(ArchiveDownloadOperation const&) = delete;
    ArchiveDownloadOperation(ArchiveDownloadOperation&&) = delete;
    ArchiveDownloadOperation& operator=(ArchiveDownloadOperation&&) = delete;

    std::expected<WorkStatus, Error> work() override;
    std::expected<WorkStatus, Error> workInStrand() override;

    bool isBarrier() const noexcept override
    {
        return false;
    }

    int parallelWorkDoable(int) const noexcept override
    {
        return 1;
    }

    SharedData::OperationType type() const override
    {
        return SharedData::OperationType::ArchiveDownload;
    }

    SecureShell::ProcessingStrand* strand() const override
    {
        return sftp_ ? sftp_->strand() : nullptr;
    }

    std::expected<void, Error> cancel(bool adoptCancelState) override;

    /**
     * @brief Ingest a preceding ScanOperation's walk of @p remoteRootPath.
     *        Called by OperationQueue's scan-complete dispatcher before this
     *        operation runs; when prepareInStrand later flattens its roots it
     *        consults the stored map and reuses these entries instead of
     *        re-recursing, so the scan work is visible (as a separate card)
     *        rather than silently buried inside prepare.
     *
     *        Safe to call multiple times — one call per directory root in
     *        @ref Options::entries.  Matching is by @p remoteRootPath which
     *        must equal the root entry's full remote path
     *        (fullPath-or-fallback-to-path in the root @ref DirectoryEntry).
     *
     *        Threading: invoked on the queue's asio strand between SFTP-strand
     *        work batches, so no extra synchronisation is required against
     *        prepareInStrand (which runs on the SFTP strand).
     */
    void setScanResultForRoot(
        std::filesystem::path const& remoteRootPath,
        std::vector<SharedData::DirectoryEntry> entries,
        std::uint64_t totalBytes
    );

  private:
    /**
     * @brief Pairs an archive-relative metadata record (what goes into the tar
     *        header) with the full remote path the operation actually opens
     *        against the SFTP session. Directory entries have an empty remote
     *        path since they carry no payload.
     */
    struct ResolvedEntry
    {
        SharedData::DirectoryEntry tarMeta{};
        std::filesystem::path remoteFullPath{};
    };

    std::expected<void, Error> prepareInStrand();
    std::expected<void, Error> flattenRootsInStrand();
    /**
     * @brief Expand a preceding ScanOperation's flat entry vector into the
     *        op's ResolvedEntry list, prefixing each entry's tar path with
     *        @p archivePrefix and composing remote paths under
     *        @p remoteRootFullPath. Index 0 (the synthetic scan root) is
     *        skipped because the root directory entry is emitted separately
     *        by flattenRootsInStrand.
     */
    void appendPrescannedInStrand(
        std::vector<SharedData::DirectoryEntry> const& entries,
        std::filesystem::path const& remoteRootFullPath,
        std::filesystem::path const& archivePrefix
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
    std::weak_ptr<SecureShell::IFileStream> currentStream_{};
    std::size_t currentEntryIndex_{0u};
    std::uint64_t currentEntryBytesRead_{0u};
    std::uint64_t totalBytesTransferred_{0u};
    std::uint64_t totalPayloadBytes_{0u};
    SecureShell::BufferLease buffer_;
    std::filesystem::path tempPath_{};
    /**
     * @brief Pre-scanned entry lists delivered by preceding ScanOperation(s),
     *        keyed by the directory root's remote full path. flattenRoots
     *        looks each directory root up here before falling back to a live
     *        recurseIntoDirectoryInStrand listing.
     */
    std::map<std::filesystem::path, std::pair<std::vector<SharedData::DirectoryEntry>, std::uint64_t>>
        prescannedRoots_{};
    /**
     * @brief Reused purely for its bytes-transferred + B/s meter machinery;
     *        the async pause/cancel side of the class is unused here because
     *        archive download drives chunks via readSomeInStrand rather than
     *        the FileStream async path that normally owns this context.
     */
    std::shared_ptr<SecureShell::AsyncTransferContext> throughputMeter_{
        std::make_shared<SecureShell::AsyncTransferContext>()};
};
