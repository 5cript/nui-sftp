#include <backend/sftp/archive_upload_operation.hpp>
#include <utility/error_message.hpp>

#include <tar_archive/error.hpp>

#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/permissions.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace
{
    /**
     * @brief ByteSink that writes through an SFTP FileStream using writeInStrand.
     *        SFTP's sftp_write does not require the file's total size in advance,
     *        so this works for streaming archive uploads.
     */
    class SftpFileByteSink final : public TarArchive::ByteSink
    {
      public:
        explicit SftpFileByteSink(std::weak_ptr<SecureShell::IFileStream> stream)
            : stream_{std::move(stream)}
        {}

        std::expected<void, TarArchive::TarError> write(std::span<std::byte const> bytes) override
        {
            if (bytes.empty())
                return {};
            auto stream = stream_.lock();
            if (!stream)
                return std::unexpected(TarArchive::makeError(
                    TarArchive::TarErrorCode::IoError, "remote SFTP stream expired mid-upload"
                ));
            const auto writeResult = stream->writeInStrand(std::string_view{
                reinterpret_cast<char const*>(bytes.data()), bytes.size(),
            });
            if (!writeResult.has_value())
                return std::unexpected(TarArchive::makeError(
                    TarArchive::TarErrorCode::IoError,
                    "SFTP writeInStrand failed: " + writeResult.error().message
                ));
            return {};
        }

        std::expected<void, TarArchive::TarError> finish() override
        {
            if (auto stream = stream_.lock(); stream)
                stream->close(false);
            return {};
        }

      private:
        std::weak_ptr<SecureShell::IFileStream> stream_;
    };

    SharedData::OperationError archiveErrorToOperationError(TarArchive::TarError const& tarError)
    {
        return SharedData::OperationError{
            .type = SharedData::OperationErrorType::UnknownError,
            .sftpError = std::nullopt,
            .extraInfo = tarError.toString(),
        };
    }

    /**
     * @brief The tar header of @p entry under @p path. Special bits never travel, and unknown permissions, as on
     *        Windows, get the defaults of the tar writer.
     */
    TarArchive::MemberHeader memberHeaderFor(std::string path, SharedData::DirectoryEntry const& entry)
    {
        auto header = TarArchive::memberHeaderOf(std::move(path), entry);
        header.permissions = Utility::sanitizePermissions(header.permissions).value_or(std::filesystem::perms::unknown);
        return header;
    }

    /**
     * @brief The metadata of @p localPath for its tar header.
     */
    std::optional<SharedData::DirectoryEntry> localPathToEntry(std::filesystem::path const& localPath)
    {
        std::error_code statErr{};
        const auto status = std::filesystem::symlink_status(localPath, statErr);
        if (statErr)
        {
            Log::warn(
                "ArchiveUploadOperation: cannot stat '{}': {}",
                Paths::nativeToUtf8(localPath),
                Utility::errorMessage(statErr)
            );
            return std::nullopt;
        }

        SharedData::DirectoryEntry entry{};
        entry.permissions = Utility::localPermissions(status);
        entry.type = SharedData::fileTypeFromStdFilesystemType(status.type());

        if (entry.type == SharedData::FileType::Regular)
        {
            std::error_code sizeErr{};
            entry.size = std::filesystem::file_size(localPath, sizeErr);
            if (sizeErr)
                entry.size = 0u;
        }

        std::error_code mtimeErr{};
        const auto fileMtime = std::filesystem::last_write_time(localPath, mtimeErr);
        if (!mtimeErr)
        {
            using namespace std::chrono;
            const auto sys = std::chrono::file_clock::to_sys(fileMtime);
            entry.mtime = static_cast<std::uint64_t>(
                duration_cast<seconds>(sys.time_since_epoch()).count()
            );
            entry.mtimeNsec = 0u;
        }
        return entry;
    }
}

bool ArchiveUploadOperation::isPayloadEntry(ResolvedEntry const& entry) const noexcept
{
    return entry.header.type == SharedData::FileType::Regular;
}

ArchiveUploadOperation::ArchiveUploadOperation(SecureShell::SftpSession& sftp, Options options)
    : Operation{}
    , sftp_{&sftp}
    , options_{std::move(options)}
{
    remoteArchivePath_ = options_.remoteArchivePath;
    tempPath_ = remoteArchivePath_.withFilenameSuffix(options_.tempFileSuffix).value_or(Paths::RemotePath{});
}

ArchiveUploadOperation::~ArchiveUploadOperation()
{
    std::ignore = cancel(false);
    waitForStrandTasks(strand());
}

std::expected<Operation::WorkStatus, Operation::Error> ArchiveUploadOperation::work()
{
    if (scanPhase_)
    {
        auto result = stepScanPhase(scanPhase_);
        if (!result.has_value())
            return enterErrorState<WorkStatus>(std::move(result).error());
        return result;
    }

    auto future = sftp_->performPromise([this]() { return workInStrand(); });
    if (future.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("ArchiveUploadOperation: work umbrella timed out.");
        return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});
    }
    return future.get();
}

std::expected<Operation::WorkStatus, Operation::Error> ArchiveUploadOperation::workInStrand()
{
    using enum OperationState;

    if (scanPhase_)
    {
        Log::error("ArchiveUploadOperation: The scan phase cannot run on the processing thread.");
        return enterErrorState<WorkStatus>(
            {.type = ErrorType::ImplementationError, .extraInfo = "Scan phase stepped on the processing thread."}
        );
    }
    if (incompleteScan_ && state_ == OperationState::NotStarted)
    {
        Log::error("ArchiveUploadOperation: The scan is incomplete: {}", incompleteScan_->toString());
        return enterErrorState<WorkStatus>(*incompleteScan_);
    }

    switch (state_)
    {
        case NotStarted:
        {
            state_ = Preparing;
            [[fallthrough]];
        }
        case Preparing:
        {
            if (options_.remoteArchivePath.empty())
                return enterErrorState<WorkStatus>({.type = ErrorType::InvalidPath});
            if (options_.localPaths.empty())
                return enterErrorState<WorkStatus>(
                    {.type = ErrorType::InvalidPath, .extraInfo = "archive has no source paths"}
                );

            const auto prepareResult = prepareInStrand();
            if (!prepareResult.has_value())
                return enterErrorState<WorkStatus>(prepareResult.error());
            state_ = Prepared;
            [[fallthrough]];
        }
        case Prepared:
        {
            state_ = Running;
            [[fallthrough]];
        }
        case Running:
        {
            if (!currentEntry_)
            {
                if (currentEntryIndex_ >= resolvedEntries_.size())
                {
                    state_ = Finalizing;
                    throughputMeter_->calculateBytesPerSecond();
                    options_.progressCallback(
                        0u,
                        totalPayloadBytes_,
                        totalPayloadBytes_,
                        throughputMeter_->bytesPerSecond()
                    );
                    [[fallthrough]];
                }
                else
                {
                    const auto openResult = openNextEntryInStrand();
                    if (!openResult.has_value())
                        return enterErrorState<WorkStatus>(openResult.error());
                    return WorkStatus::MoreWork;
                }
            }
            else
            {
                const auto chunkResult = readChunkInStrand();
                if (!chunkResult.has_value())
                    return enterErrorState<WorkStatus>(chunkResult.error());
                return WorkStatus::MoreWork;
            }
        }
        case Finalizing:
        {
            const auto finaliseResult = finaliseArchiveInStrand();
            if (!finaliseResult.has_value())
                return enterErrorState<WorkStatus>(finaliseResult.error());
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case Completed:
        {
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case Failed:
        {
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case Canceled:
        {
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case PartialSuccess:
        {
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, Operation::Error> ArchiveUploadOperation::flattenRoots()
{
    resolvedEntries_.clear();
    resolvedEntries_.reserve(options_.localPaths.size());

    for (auto const& localRoot : options_.localPaths)
    {
        const auto name = localRoot.filename();
        if (!name)
        {
            Log::error("ArchiveUploadOperation: refusing root '{}', it has no name", localRoot.toUtf8());
            return std::unexpected(Error{
                .type = ErrorType::InvalidPath,
                .extraInfo = fmt::format("'{}' cannot be archived under its own name", localRoot.toUtf8()),
            });
        }
        const auto archivePrefix = Paths::RelativePath{} / *name;
        auto rootEntry = localPathToEntry(localRoot.native());
        if (!rootEntry)
            continue;

        if (rootEntry->type == SharedData::FileType::Regular)
        {
            totalPayloadBytes_ += rootEntry->size;
            resolvedEntries_.push_back(
                ResolvedEntry{
                    .header = memberHeaderFor(archivePrefix.value(), *rootEntry),
                    .source = localRoot,
                }
            );
        }
        else if (rootEntry->type == SharedData::FileType::Directory)
        {
            rootEntry->size = 0u;
            resolvedEntries_.push_back(
                ResolvedEntry{.header = memberHeaderFor(archivePrefix.value(), *rootEntry)}
            );

            // Directory roots are expanded exclusively from a preceding
            // LocalScanOperation's results — the archive op no longer
            // recurses itself, so the scan phase is always a visible card.
            const auto it = prescannedRoots_.find(localRoot);
            if (it == prescannedRoots_.end())
            {
                Log::error(
                    "ArchiveUploadOperation: missing pre-scanned entries for directory root '{}'", localRoot.toUtf8()
                );
                return std::unexpected(Error{
                    .type = ErrorType::ImplementationError,
                    .extraInfo = fmt::format("no pre-scanned entries for directory root '{}'", localRoot.toUtf8()),
                });
            }
            appendPrescannedFromScan(it->second.first, localRoot, archivePrefix);
        }
        else
        {
            Log::warn(
                "ArchiveUploadOperation: skipping non-regular, non-directory root '{}' (type={})",
                localRoot.toUtf8(),
                static_cast<int>(rootEntry->type)
            );
        }
    }
    return {};
}

void ArchiveUploadOperation::appendPrescannedFromScan(
    std::vector<SharedData::DirectoryEntry> const& entries,
    Paths::LocalPath const& localRoot,
    Paths::RelativePath const& archivePrefix
)
{
    // Index 0 is the synthetic scan root (the directory itself); the caller
    // emitted its tar header already, so we start at 1 and consume the
    // walker's flat depth-first entry list — mirrors the download op's
    // appendPrescannedInStrand.
    for (std::size_t idx = 1u; idx < entries.size(); ++idx)
    {
        auto const& child = entries[idx];
        auto source = SharedData::relativePath(entries, child)
                          .and_then(
                              [&localRoot](Paths::RelativePath const& relative)
                              {
                                  return localRoot.join(relative);
                              }
                          );
        if (!source)
        {
            Log::warn("ArchiveUploadOperation: skipping pre-scanned child: {}", source.error().toString());
            continue;
        }
        const auto memberPath = archivePrefix / *SharedData::relativePath(entries, child);

        if (child.type == SharedData::FileType::Regular)
        {
            totalPayloadBytes_ += child.size;
            resolvedEntries_.push_back(
                ResolvedEntry{
                    .header = memberHeaderFor(memberPath.value(), child),
                    .source = *std::move(source),
                }
            );
        }
        else if (child.type == SharedData::FileType::Directory)
        {
            ResolvedEntry dirEntry{.header = memberHeaderFor(memberPath.value(), child)};
            dirEntry.header.size = 0u;
            resolvedEntries_.push_back(std::move(dirEntry));
        }
        else
        {
            Log::warn(
                "ArchiveUploadOperation: skipping non-regular pre-scanned child '{}' (type={})",
                source->toUtf8(),
                static_cast<int>(child.type)
            );
        }
    }
}

std::expected<void, Operation::Error> ArchiveUploadOperation::prepareInStrand()
{
    if (remoteArchivePath_.empty() || remoteArchivePath_.isRoot() || tempPath_.empty())
    {
        return std::unexpected(
            Error{
                .type = ErrorType::InvalidPath,
                .extraInfo = options_.remoteArchivePath.value(),
            }
        );
    }

    const auto flattenResult = flattenRoots();
    if (!flattenResult.has_value())
        return std::unexpected(flattenResult.error());

    if (resolvedEntries_.empty())
        return std::unexpected(Error{
            .type = ErrorType::FileNotFound,
            .extraInfo = "no local files could be stat'd",
        });

    // Fail early instead of packing everything first, the final rename refuses to replace it anyway.
    if (!options_.mayOverwrite && sftp_->lstatInStrand(remoteArchivePath_).has_value())
        return std::unexpected(Error{.type = ErrorType::FileExists});

    // Written under the temp name, so a failed or cancelled upload never leaves a truncated destination.
    using OpenType = SecureShell::SftpSession::OpenType;
    const auto openResult = sftp_->openFileInStrand(
        tempPath_, OpenType::Write | OpenType::Create | OpenType::Truncate, std::filesystem::perms{0644u}
    );
    if (!openResult.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: cannot open remote '{}': {}",
            options_.remoteArchivePath.value(),
            openResult.error().message
        );
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = openResult.error(),
                .extraInfo = fmt::format("Opening remote archive file: {}", options_.remoteArchivePath.value()),
            }
        );
    }
    remoteStream_ = openResult.value();
    tempCreated_ = true;

    // Wrap the SFTP terminal sink in the requested codec. Auto means derive
    // from the user-facing remote path so the bytes on the wire match the
    // declared file extension.
    const auto resolvedCodec = options_.compression == TarArchive::Compression::Auto
        ? TarArchive::compressionFromExtension(Paths::nativeFromUtf8(options_.remoteArchivePath.value()))
        : options_.compression;

    auto wrappedSink = TarArchive::wrapSinkWithCodec(
        std::make_unique<SftpFileByteSink>(remoteStream_),
        resolvedCodec,
        options_.compressionOptions
    );
    if (!wrappedSink)
    {
        Log::error(
            "ArchiveUploadOperation: cannot wrap remote sink with codec: {}",
            wrappedSink.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(wrappedSink.error()));
    }

    writer_ = TarArchive::Writer::makeFromSink(std::move(*wrappedSink));

    buffer_ = sftp_->bufferProvider().lease(totalPayloadBytes_);
    if (buffer_.empty())
    {
        Log::error("ArchiveUploadOperation: No transfer buffer available from pool.");
        return std::unexpected(Error{
            .type = ErrorType::SftpError,
            .sftpError = SecureShell::SftpError{
                .message = "No buffer available from pool",
                .wrapperError = SecureShell::WrapperErrors::BufferUnavailable,
            },
        });
    }
    return {};
}

std::expected<void, Operation::Error> ArchiveUploadOperation::openNextEntryInStrand()
{
    auto& resolved = resolvedEntries_[currentEntryIndex_];

    if (!isPayloadEntry(resolved))
    {
        // Directory entry — emit the tar header only and advance.
        auto entryOrError = writer_->beginEntry(resolved.header);
        if (!entryOrError.has_value())
        {
            Log::error(
                "ArchiveUploadOperation: cannot begin tar entry '{}': {}",
                resolved.header.path,
                entryOrError.error().toString()
            );
            return std::unexpected(archiveErrorToOperationError(entryOrError.error()));
        }
        const auto closeResult = std::move(*entryOrError).close();
        if (!closeResult.has_value())
            return std::unexpected(archiveErrorToOperationError(closeResult.error()));
        ++currentEntryIndex_;
        return {};
    }

    currentLocalFile_.open(resolved.source.native(), std::ios::binary);
    if (!currentLocalFile_.is_open())
    {
        Log::error(
            "ArchiveUploadOperation: cannot open local '{}'",
            resolved.source.toUtf8()
        );
        return std::unexpected(Error{
            .type = ErrorType::OpenFailure,
            .extraInfo = resolved.source.toUtf8(),
        });
    }

    auto entryWriterOrError = writer_->beginEntry(resolved.header);
    if (!entryWriterOrError.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: cannot begin tar entry '{}': {}",
            resolved.header.path,
            entryWriterOrError.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(entryWriterOrError.error()));
    }
    currentEntry_.emplace(std::move(*entryWriterOrError));
    currentEntryBytesRead_ = 0u;
    return {};
}

std::expected<bool, Operation::Error> ArchiveUploadOperation::readChunkInStrand()
{
    auto const& resolved = resolvedEntries_[currentEntryIndex_];
    const std::uint64_t remaining = resolved.header.size - currentEntryBytesRead_;
    if (remaining == 0u)
    {
        const auto closeResult = finaliseEntryInStrand();
        if (!closeResult.has_value())
            return std::unexpected(closeResult.error());
        return true;
    }

    const std::size_t requestBytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer_.size(), remaining)
    );
    currentLocalFile_.read(buffer_.data(), static_cast<std::streamsize>(requestBytes));
    const auto bytesRead = static_cast<std::size_t>(currentLocalFile_.gcount());
    if (bytesRead == 0u)
    {
        Log::error(
            "ArchiveUploadOperation: local '{}' truncated mid-read (expected {} more bytes)",
            resolved.source.toUtf8(),
            remaining
        );
        return std::unexpected(Error{
            .type = ErrorType::SizeMismatch,
            .extraInfo = fmt::format(
                "'{}' ended {} bytes early", resolved.source.toUtf8(), remaining
            ),
        });
    }

    const auto writeResult = currentEntry_->write(std::span<std::byte const>{
        reinterpret_cast<std::byte const*>(buffer_.data()), bytesRead,
    });
    if (!writeResult.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: tar write failure on '{}': {}",
            resolved.header.path,
            writeResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(writeResult.error()));
    }

    currentEntryBytesRead_ += bytesRead;
    totalBytesTransferred_ += bytesRead;
    throughputMeter_->addBytesTransferred(
        static_cast<SecureShell::AsyncTransferContext::SignedSizeType>(bytesRead)
    );
    throughputMeter_->calculateBytesPerSecond();
    options_.progressCallback(
        0u, totalPayloadBytes_, totalBytesTransferred_, throughputMeter_->bytesPerSecond()
    );

    if (currentEntryBytesRead_ >= resolved.header.size)
    {
        const auto closeResult = finaliseEntryInStrand();
        if (!closeResult.has_value())
            return std::unexpected(closeResult.error());
    }
    return true;
}

std::expected<void, Operation::Error> ArchiveUploadOperation::finaliseEntryInStrand()
{
    if (!currentEntry_)
        return {};

    auto handle = std::move(*currentEntry_);
    currentEntry_.reset();
    currentLocalFile_.close();

    auto const& resolved = resolvedEntries_[currentEntryIndex_];
    if (currentEntryBytesRead_ != resolved.header.size)
    {
        Log::error(
            "ArchiveUploadOperation: entry '{}' is incomplete ({} of {} bytes)",
            resolved.header.path,
            currentEntryBytesRead_,
            resolved.header.size
        );
        return std::unexpected(Error{
            .type = ErrorType::SizeMismatch,
            .extraInfo = fmt::format(
                "'{}' holds {} of {} bytes",
                resolved.header.path,
                currentEntryBytesRead_,
                resolved.header.size
            ),
        });
    }

    const auto closeResult = std::move(handle).close();
    if (!closeResult.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: tar entry close failed for '{}': {}",
            resolved.header.path,
            closeResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(closeResult.error()));
    }
    ++currentEntryIndex_;
    return {};
}

std::expected<void, Operation::Error> ArchiveUploadOperation::finaliseArchiveInStrand()
{
    if (!writer_)
        return std::unexpected(Error{
            .type = ErrorType::ImplementationError,
            .extraInfo = "archive writer is not open",
        });

    const auto finaliseResult = writer_->finalize();
    writer_.reset();
    if (!finaliseResult.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: writer finalize failed: {}",
            finaliseResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(finaliseResult.error()));
    }

    if (auto stream = remoteStream_.lock(); stream)
        stream->close(false);
    remoteStream_.reset();

    const auto moved = sftp.moveIntoPlaceInStrand(
        tempPath_,
        remoteArchivePath_,
        options_.mayOverwrite,
        Persistence::asideFileSuffix(options_.tempFileSuffix),
        SharedData::FileType::Regular
    );
    if (!moved.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: cannot move the archive to '{}': {}",
            options_.remoteArchivePath.value(),
            moved.error().message
        );
        return std::unexpected(remoteMoveError(moved.error()));
    }
    tempCreated_ = false;
    return {};
}

std::expected<void, Operation::Error> ArchiveUploadOperation::cancel(bool adoptCancelState)
{
    if (scanPhase_)
        scanPhase_->cancel(adoptCancelState);
    if (adoptCancelState)
    {
        Log::info(
            "ArchiveUploadOperation: cancelled ({} roots, {} bytes planned).",
            options_.localPaths.size(),
            totalPayloadBytes_
        );
        state_ = OperationState::Canceled;
    }
    cleanup();
    return {};
}

void ArchiveUploadOperation::setScanResultForRoot(
    Paths::LocalPath const& localRootPath,
    std::vector<SharedData::DirectoryEntry> entries,
    std::uint64_t totalBytes
)
{
    prescannedRoots_.insert_or_assign(
        localRootPath, std::make_pair(std::move(entries), totalBytes)
    );
}

void ArchiveUploadOperation::cleanup()
{
    currentEntry_.reset();
    currentLocalFile_.close();
    writer_.reset();
    if (auto stream = remoteStream_.lock(); stream)
        stream->close(false);
    remoteStream_.reset();

    if (tempCreated_)
    {
        tempCreated_ = false;
        if (auto* processingStrand = sftp_->strand(); processingStrand && processingStrand->withinProcessingThread())
            std::ignore = sftp_->removeFileInStrand(tempPath_);
        else
            std::ignore = sftp_->removeFile(tempPath_).wait_for(options_.futureTimeout);
    }
}

void ArchiveUploadOperation::addScanRoot(std::unique_ptr<LocalScanOperation> scan)
{
    if (!scanPhase_)
    {
        scanPhase_ = std::make_unique<ScanPhase<LocalScanOperation>>(
            [this](LocalScanOperation& finished)
            {
                const auto scannedBytes = finished.totalBytes();
                // An archive cannot be partly successful, missing content fails it.
                if (const auto& unreadable = finished.unreadableDirectories(); !unreadable.empty() && !incompleteScan_)
                {
                    incompleteScan_ = Error{
                        .type = ErrorType::DirectoryUnreadable,
                        .extraInfo = fmt::format("Cannot list '{}'", unreadable.front()),
                    };
                }
                if (const auto rejected = finished.rejectedPaths(); !rejected.empty() && !incompleteScan_)
                {
                    incompleteScan_ = Error{
                        .type = ErrorType::InvalidName,
                        .extraInfo = fmt::format("'{}' is not a valid name", rejected.front()),
                    };
                }
                setScanResultForRoot(finished.localPath(), finished.ejectEntries(), scannedBytes);
            }
        );
    }
    scanPhase_->addRoot(std::move(scan));
}
