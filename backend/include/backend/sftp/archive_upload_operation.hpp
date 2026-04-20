#pragma once

#include <backend/sftp/operation.hpp>
#include <ssh/async/buffer_provider.hpp>
#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <tar_archive/archive.hpp>
#include <tar_archive/writer.hpp>
#include <tar_archive/entry_writer.hpp>
#include <tar_archive/compression.hpp>
#include <shared_data/directory_entry.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
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
        std::vector<std::filesystem::path> localPaths{};

        /** @brief Destination archive path on the remote side. */
        std::filesystem::path remoteArchivePath{};

        /** @brief Compression codec; Auto means derive from remoteArchivePath extension. */
        TarArchive::Compression compression{TarArchive::Compression::Auto};

        /** @brief Codec-specific tuning. */
        TarArchive::CompressionOptions compressionOptions{};

        /** @brief Refuse to overwrite an existing remote file. */
        bool mayOverwrite{false};

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
        return SharedData::OperationType::ArchiveUpload;
    }

    SecureShell::ProcessingStrand* strand() const override
    {
        return sftp_ ? sftp_->strand() : nullptr;
    }

    std::expected<void, Error> cancel(bool adoptCancelState) override;

  private:
    /**
     * @brief Archive-relative metadata paired with the full local source path.
     *        Directory entries have an empty localFullPath since they carry no
     *        payload.
     */
    struct ResolvedEntry
    {
        SharedData::DirectoryEntry tarMeta{};
        std::filesystem::path localFullPath{};
    };

    std::expected<void, Error> prepareInStrand();
    void flattenRoots();
    void recurseIntoDirectory(
        std::filesystem::path const& localRoot, std::filesystem::path const& archivePrefix
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
};
