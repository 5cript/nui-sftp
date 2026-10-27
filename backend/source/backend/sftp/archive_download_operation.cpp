#include <backend/sftp/archive_download_operation.hpp>
#include <utility/error_message.hpp>
#include <utility/download_mark.hpp>
#include <paths/native.hpp>
#include <persistence/state/sftp_options.hpp>

#include <tar_archive/error.hpp>

#include <log/log.hpp>
#include <utility/rename.hpp>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>

namespace
{
    /**
     * @brief ByteSink implementation that writes into a local std::ofstream owned
     *        by the sink. Used as the terminal sink of a TarArchive::Writer when we
     *        download straight to a local file.
     */
    class LocalFileByteSink final : public TarArchive::ByteSink
    {
      public:
        explicit LocalFileByteSink(std::filesystem::path const& targetPath)
        {
            output_.open(targetPath, std::ios::binary | std::ios::trunc);
        }

        bool isOpen() const
        {
            return output_.is_open();
        }

        std::expected<void, TarArchive::TarError> write(std::span<std::byte const> bytes) override
        {
            if (!output_.is_open())
                return std::unexpected(TarArchive::makeError(
                    TarArchive::TarErrorCode::IoError, "local archive file is not open"
                ));
            if (bytes.empty())
                return {};
            output_.write(
                reinterpret_cast<char const*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
            if (!output_)
                return std::unexpected(TarArchive::makeError(
                    TarArchive::TarErrorCode::IoError, "local archive write failed", errno
                ));
            return {};
        }

        std::expected<void, TarArchive::TarError> finish() override
        {
            output_.flush();
            if (!output_)
                return std::unexpected(TarArchive::makeError(
                    TarArchive::TarErrorCode::IoError, "local archive flush failed", errno
                ));
            output_.close();
            return {};
        }

      private:
        std::ofstream output_{};
    };

    /**
     * @brief Open a sink for the actual on-disk file.
     *
     * @p archivePath is where the bytes are written (e.g. the .filepart temp).
     * @p codecHintPath is the path consulted for codec auto-detection — must be
     * the user-facing final path so the .filepart suffix doesn't fool extension
     * parsing.
     */
    std::expected<std::unique_ptr<TarArchive::ByteSink>, TarArchive::TarError>
    openArchiveSink(
        std::filesystem::path const& archivePath,
        std::filesystem::path const& codecHintPath,
        TarArchive::Compression compression,
        TarArchive::CompressionOptions const& compressionOptions
    )
    {
        const TarArchive::Compression resolved =
            compression == TarArchive::Compression::Auto
                ? TarArchive::compressionFromExtension(codecHintPath)
                : compression;
        return TarArchive::makeSink(archivePath, resolved, compressionOptions);
    }

    SharedData::OperationError archiveErrorToOperationError(TarArchive::TarError const& tarError)
    {
        return SharedData::OperationError{
            .type = SharedData::OperationErrorType::UnknownError,
            .sftpError = std::nullopt,
            .extraInfo = tarError.toString(),
        };
    }

    /**
     * @brief True for entries whose payload needs to stream over SFTP (regular
     *        files only; directories and other typeflags carry no payload).
     */
    bool isRegularFile(TarArchive::MemberHeader const& entry) noexcept
    {
        return entry.type == SharedData::FileType::Regular;
    }
}

bool ArchiveDownloadOperation::isPayloadEntry(ResolvedEntry const& entry) const noexcept
{
    return isRegularFile(entry.header);
}

ArchiveDownloadOperation::ArchiveDownloadOperation(std::weak_ptr<SecureShell::SftpSession> sftp, Options options)
    : Operation{}
    , sftp_{std::move(sftp)}
    , options_{std::move(options)}
{
    options_.tempFileSuffix = Persistence::effectiveTempFileSuffix(options_.tempFileSuffix);

    if (!options_.localArchivePath.empty())
    {
        localArchiveNativePath_ = options_.localArchivePath.native();
        tempPath_ = localArchiveNativePath_;
        tempPath_ += options_.tempFileSuffix;
    }
}

ArchiveDownloadOperation::~ArchiveDownloadOperation()
{
    std::ignore = cancel(false);
    waitForStrandTasks(strand());
}

std::expected<Operation::WorkStatus, Operation::Error> ArchiveDownloadOperation::work()
{
    if (scanPhase_)
    {
        auto result = stepScanPhase(scanPhase_);
        if (!result.has_value())
            return enterErrorState<WorkStatus>(std::move(result).error());
        return result;
    }

    const auto sftp = openSession(sftp_);
    if (!sftp)
        return enterErrorState<WorkStatus>(channelClosedError());
    auto future = sftp->performPromise(
        [this]()
        {
            return workInStrand();
        }
    );
    if (future.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("ArchiveDownloadOperation: work umbrella timed out.");
        return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});
    }
    return future.get();
}

std::expected<Operation::WorkStatus, Operation::Error>
ArchiveDownloadOperation::workInStrand()
{
    using enum OperationState;

    if (scanPhase_)
    {
        Log::error("ArchiveDownloadOperation: The scan phase cannot run on the processing thread.");
        return enterErrorState<WorkStatus>(
            {.type = ErrorType::ImplementationError, .extraInfo = "Scan phase stepped on the processing thread."}
        );
    }

    const auto sftp = openSession(sftp_);
    if (!sftp && !hasEnded())
        return enterErrorState<WorkStatus>(channelClosedError());

    switch (state_)
    {
        case NotStarted:
        {
            state_ = Preparing;
            [[fallthrough]];
        }
        case Preparing:
        {
            if (options_.localArchivePath.empty())
                return enterErrorState<WorkStatus>({.type = ErrorType::InvalidPath});
            if (options_.roots.empty())
                return enterErrorState<WorkStatus>(
                    {.type = ErrorType::ImplementationError, .extraInfo = "archive has no roots"}
                );
            if (std::error_code existsError{}; std::filesystem::exists(localArchiveNativePath_, existsError) && !options_.mayOverwrite)
                return enterErrorState<WorkStatus>({.type = ErrorType::FileExists});

            const auto prepareResult = prepareInStrand(*sftp);
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
                    const auto openResult = openNextEntryInStrand(*sftp);
                    if (!openResult.has_value())
                        return enterErrorState<WorkStatus>(openResult.error());
                    return WorkStatus::MoreWork;
                }
            }
            else
            {
                const auto readResult = readChunkInStrand();
                if (!readResult.has_value())
                    return enterErrorState<WorkStatus>(readResult.error());
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
            Log::warn("ArchiveDownloadOperation: already completed.");
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

std::expected<void, Operation::Error> ArchiveDownloadOperation::prepareInStrand(SecureShell::SftpSession& sftp)
{
    if (options_.createMissingDirectories)
    {
        std::error_code mkErr{};
        std::filesystem::create_directories(tempPath_.parent_path(), mkErr);
        if (mkErr)
        {
            Log::error(
                "ArchiveDownloadOperation: cannot create parents of '{}': {}",
                Paths::nativeToUtf8(tempPath_),
                Utility::errorMessage(mkErr)
            );
            return std::unexpected(Error{.type = ErrorType::CannotCreateDirectory});
        }
    }

    const auto flattenResult = flattenRootsInStrand(sftp);
    if (!flattenResult.has_value())
        return std::unexpected(flattenResult.error());

    if (resolvedEntries_.empty())
        return std::unexpected(Error{
            .type = ErrorType::FileNotFound,
            .extraInfo = "no archivable entries under the selected roots",
        });

    // Opening for writing follows a symlink planted at the temp path, out of the target directory.
    if (std::error_code linkError{}; std::filesystem::is_symlink(std::filesystem::symlink_status(tempPath_, linkError)))
    {
        Log::error("ArchiveDownloadOperation: temp path '{}' is a symlink.", Paths::nativeToUtf8(tempPath_));
        return std::unexpected(Error{.type = ErrorType::OpenFailure, .extraInfo = "The temporary file path is a symlink"});
    }

    auto sinkOrError =
        openArchiveSink(tempPath_, localArchiveNativePath_, options_.compression, options_.compressionOptions);
    if (!sinkOrError.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: cannot open archive sink: {}",
            sinkOrError.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(sinkOrError.error()));
    }
    writer_ = TarArchive::Writer::makeFromSink(std::move(*sinkOrError));

    buffer_ = sftp.bufferProvider().lease(totalPayloadBytes_);
    if (buffer_.empty())
    {
        Log::error("ArchiveDownloadOperation: No transfer buffer available from pool.");
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

std::expected<void, Operation::Error> ArchiveDownloadOperation::flattenRootsInStrand(SecureShell::SftpSession& sftp)
{
    for (auto const& root : options_.roots)
    {
        const auto name = root.path.filename();
        if (!name)
        {
            Log::error("ArchiveDownloadOperation: refusing root '{}', it has no name", root.path.value());
            return std::unexpected(Error{
                .type = ErrorType::InvalidPath,
                .extraInfo = fmt::format("'{}' cannot be archived under its own name", root.path.value()),
            });
        }
        const auto archivePrefix = Paths::RelativePath{} / *name;

        if (root.entry.type == SharedData::FileType::Regular)
        {
            // The listing the root came from may be old, the header must carry the size that is read.
            const auto current = sftp.lstatInStrand(root.path);
            if (!current)
            {
                Log::error(
                    "ArchiveDownloadOperation: cannot look up root '{}': {}",
                    root.path.value(),
                    current.error().toString()
                );
                return std::unexpected(
                    Error{
                        .type = ErrorType::FileStatFailed,
                        .sftpError = current.error(),
                        .extraInfo = root.path.value(),
                    }
                );
            }
            if (!current->isRegularFile())
            {
                Log::error("ArchiveDownloadOperation: root '{}' is no longer a regular file", root.path.value());
                return std::unexpected(Error{.type = ErrorType::NotAFile, .extraInfo = root.path.value()});
            }
            ResolvedEntry resolved{
                .header = TarArchive::memberHeaderOf(archivePrefix.value(), *current),
                .source = root.path,
            };
            totalPayloadBytes_ += resolved.header.size;
            resolvedEntries_.push_back(std::move(resolved));
        }
        else if (root.entry.type == SharedData::FileType::Directory)
        {
            ResolvedEntry dirEntry{.header = TarArchive::memberHeaderOf(archivePrefix.value(), root.entry)};
            dirEntry.header.size = 0u;
            resolvedEntries_.push_back(std::move(dirEntry));

            // Directory roots are expanded exclusively from a preceding
            // ScanOperation's results — there is no self-contained recurse
            // fallback, so a visible scan card is always the source of truth
            // for this op's entry list.
            const auto it = prescannedRoots_.find(root.path);
            if (it == prescannedRoots_.end())
            {
                Log::error(
                    "ArchiveDownloadOperation: missing pre-scanned entries for directory root '{}'", root.path.value()
                );
                return std::unexpected(Error{
                    .type = ErrorType::ImplementationError,
                    .extraInfo = fmt::format("no pre-scanned entries for directory root '{}'", root.path.value()),
                });
            }
            appendPrescannedInStrand(it->second.first, root.path, archivePrefix);
        }
        else
        {
            Log::warn(
                "ArchiveDownloadOperation: skipping non-regular, non-directory root '{}' (type={})",
                root.path.value(),
                static_cast<int>(root.entry.type)
            );
        }
    }
    return {};
}

void ArchiveDownloadOperation::appendPrescannedInStrand(
    std::vector<SharedData::DirectoryEntry> const& entries,
    Paths::RemotePath const& remoteRoot,
    Paths::RelativePath const& archivePrefix
)
{
    // Index 0 is the synthetic scan root (the directory itself); the caller
    // emitted its tar header already, so we start at 1 and consume the
    // walker's flat depth-first entry list.
    for (std::size_t idx = 1u; idx < entries.size(); ++idx)
    {
        auto const& child = entries[idx];
        const auto relative = SharedData::relativePath(entries, child);
        if (!relative)
        {
            Log::warn("ArchiveDownloadOperation: skipping pre-scanned child: {}", relative.error().toString());
            continue;
        }
        const auto memberPath = archivePrefix / *relative;
        const auto source = remoteRoot / *relative;

        if (child.type == SharedData::FileType::Regular)
        {
            ResolvedEntry resolved{
                .header = TarArchive::memberHeaderOf(memberPath.value(), child),
                .source = source,
            };
            totalPayloadBytes_ += resolved.header.size;
            resolvedEntries_.push_back(std::move(resolved));
        }
        else if (child.type == SharedData::FileType::Directory)
        {
            ResolvedEntry dirEntry{.header = TarArchive::memberHeaderOf(memberPath.value(), child)};
            dirEntry.header.size = 0u;
            resolvedEntries_.push_back(std::move(dirEntry));
        }
        else
        {
            Log::warn(
                "ArchiveDownloadOperation: skipping non-regular pre-scanned child '{}' (type={})",
                source.value(),
                static_cast<int>(child.type)
            );
        }
    }
}

std::expected<void, Operation::Error> ArchiveDownloadOperation::openNextEntryInStrand(SecureShell::SftpSession& sftp)
{
    if (!writer_)
        return std::unexpected(Error{
            .type = ErrorType::ImplementationError,
            .extraInfo = "archive writer is not open",
        });

    auto& resolved = resolvedEntries_[currentEntryIndex_];

    if (!isPayloadEntry(resolved))
    {
        // Directory (or other payload-less) entry — just emit the tar header
        // and move on without opening a remote stream.
        auto entryOrError = writer_->beginEntry(resolved.header);
        if (!entryOrError.has_value())
        {
            Log::error(
                "ArchiveDownloadOperation: cannot begin tar entry '{}': {}",
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

    const auto streamOpenResult = sftp.openFileInStrand(
        resolved.source, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::unknown
    );
    if (!streamOpenResult.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: cannot open remote '{}': {}",
            resolved.source.value(),
            streamOpenResult.error().message
        );
        return std::unexpected(Error{
            .type = ErrorType::SftpError,
            .sftpError = streamOpenResult.error(),
            .extraInfo = fmt::format(
                "Opening remote file: {}", resolved.source.value()
            ),
        });
    }
    currentStream_ = streamOpenResult.value();

    auto entryWriterOrError = writer_->beginEntry(resolved.header);
    if (!entryWriterOrError.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: cannot begin tar entry '{}': {}",
            resolved.header.path,
            entryWriterOrError.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(entryWriterOrError.error()));
    }
    currentEntry_.emplace(std::move(*entryWriterOrError));
    currentEntryBytesRead_ = 0u;
    return {};
}

std::expected<bool, Operation::Error> ArchiveDownloadOperation::readChunkInStrand()
{
    auto stream = currentStream_.lock();
    if (!stream)
    {
        Log::error("ArchiveDownloadOperation: remote stream expired mid-entry.");
        return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
    }
    if (!currentEntry_)
        return std::unexpected(Error{
            .type = ErrorType::ImplementationError,
            .extraInfo = "no active tar entry",
        });

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
    const auto readResult = stream->readSomeInStrand(buffer_.data(), requestBytes);
    if (!readResult.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: read failure on '{}': {}",
            resolved.source.value(),
            readResult.error().message
        );
        return std::unexpected(Error{
            .type = ErrorType::SftpError,
            .sftpError = readResult.error(),
            .extraInfo = fmt::format(
                "Reading remote file: {}", resolved.source.value()
            ),
        });
    }
    const auto bytesRead = readResult.value();
    if (bytesRead == 0)
    {
        Log::error(
            "ArchiveDownloadOperation: premature EOF on '{}' (expected {} more bytes)",
            resolved.source.value(),
            remaining
        );
        return std::unexpected(Error{
            .type = ErrorType::SizeMismatch,
            .extraInfo = fmt::format(
                "'{}' ended {} bytes early", resolved.source.value(), remaining
            ),
        });
    }

    const auto writeResult = currentEntry_->write(std::span<std::byte const>{
        reinterpret_cast<std::byte const*>(buffer_.data()),
        static_cast<std::size_t>(bytesRead),
    });
    if (!writeResult.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: tar write failure on '{}': {}",
            resolved.header.path,
            writeResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(writeResult.error()));
    }

    currentEntryBytesRead_ += static_cast<std::uint64_t>(bytesRead);
    totalBytesTransferred_ += static_cast<std::uint64_t>(bytesRead);
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

std::expected<void, Operation::Error> ArchiveDownloadOperation::finaliseEntryInStrand()
{
    if (!currentEntry_)
        return {};

    auto handle = std::move(*currentEntry_);
    currentEntry_.reset();

    if (auto stream = currentStream_.lock(); stream)
    {
        stream->close(false);
    }
    currentStream_.reset();

    auto const& resolved = resolvedEntries_[currentEntryIndex_];
    if (currentEntryBytesRead_ != resolved.header.size)
    {
        Log::error(
            "ArchiveDownloadOperation: entry '{}' is incomplete ({} of {} bytes)",
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
            "ArchiveDownloadOperation: tar entry close failed for '{}': {}",
            resolved.header.path,
            closeResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(closeResult.error()));
    }
    ++currentEntryIndex_;
    return {};
}

std::expected<void, Operation::Error> ArchiveDownloadOperation::finaliseArchiveInStrand()
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
            "ArchiveDownloadOperation: writer finalize failed: {}",
            finaliseResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(finaliseResult.error()));
    }

    if (options_.markAsDownloaded)
    {
        if (const auto marked = Utility::markAsDownloaded(tempPath_); marked)
            Log::warn(
                "ArchiveDownloadOperation: Cannot mark the archive as downloaded: {}", Utility::errorMessage(marked)
            );
    }

    if (const auto moved = Utility::moveIntoPlace(tempPath_, localArchiveNativePath_, options_.mayOverwrite); !moved)
    {
        Log::error(
            "ArchiveDownloadOperation: Failed to move the archive into place: {}", Utility::errorMessage(moved.error())
        );
        return std::unexpected(localMoveError(moved.error()));
    }
    return {};
}

std::expected<void, Operation::Error> ArchiveDownloadOperation::cancel(bool adoptCancelState)
{
    if (scanPhase_)
        scanPhase_->cancel(adoptCancelState);
    if (adoptCancelState)
    {
        Log::info(
            "ArchiveDownloadOperation: cancelled ({} roots, {} bytes planned).",
            options_.roots.size(),
            totalPayloadBytes_
        );
        state_ = OperationState::Canceled;
    }
    cleanup();
    return {};
}

void ArchiveDownloadOperation::setScanResultForRoot(
    Paths::RemotePath const& remoteRootPath,
    std::vector<SharedData::DirectoryEntry> entries,
    std::uint64_t totalBytes
)
{
    prescannedRoots_.insert_or_assign(
        remoteRootPath, std::make_pair(std::move(entries), totalBytes)
    );
}

void ArchiveDownloadOperation::cleanup()
{
    // The temp file goes anyway: padding the open entry would compress and write its whole remainder.
    if (writer_)
        writer_->discard();
    currentEntry_.reset();
    if (auto stream = currentStream_.lock(); stream)
        stream->close(false);
    currentStream_.reset();
    writer_.reset();

    if (!tempPath_.empty())
    {
        std::error_code removeError{};
        std::filesystem::remove(tempPath_, removeError);
        if (removeError)
            Log::warn(
                "ArchiveDownloadOperation: Cannot remove '{}': {}",
                Paths::nativeToUtf8(tempPath_),
                Utility::errorMessage(removeError)
            );
    }
}

void ArchiveDownloadOperation::addScanRoot(std::unique_ptr<ScanOperation> scan)
{
    if (!scanPhase_)
    {
        scanPhase_ = std::make_unique<ScanPhase<ScanOperation>>(
            [this](ScanOperation& finished)
            {
                const auto scannedBytes = finished.totalBytes();
                setScanResultForRoot(finished.remotePath(), finished.ejectEntries(), scannedBytes);
            }
        );
    }
    scanPhase_->addRoot(std::move(scan));
}
