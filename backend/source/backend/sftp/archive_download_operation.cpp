#include <backend/sftp/archive_download_operation.hpp>

#include <tar_archive/error.hpp>

#include <log/log.hpp>

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
    bool isRegularFile(SharedData::DirectoryEntry const& entry) noexcept
    {
        return entry.type == SharedData::FileType::Regular;
    }
}

bool ArchiveDownloadOperation::isPayloadEntry(ResolvedEntry const& entry) const noexcept
{
    return isRegularFile(entry.tarMeta);
}

ArchiveDownloadOperation::ArchiveDownloadOperation(SecureShell::SftpSession& sftp, Options options)
    : Operation{}
    , sftp_{&sftp}
    , options_{std::move(options)}
{
    if (options_.tempFileSuffix.empty())
        options_.tempFileSuffix = ".filepart";

    tempPath_ = options_.localArchivePath;
    tempPath_ += options_.tempFileSuffix;
}

ArchiveDownloadOperation::~ArchiveDownloadOperation()
{
    std::ignore = cancel(false);

    if (auto stream = currentStream_.lock(); stream)
    {
        stream->strand()->pushPromiseTask([]() {}).get();
    }
}

std::expected<Operation::WorkStatus, Operation::Error> ArchiveDownloadOperation::work()
{
    auto future = sftp_->performPromise([this]() { return workInStrand(); });
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
            if (options_.entries.empty())
                return enterErrorState<WorkStatus>(
                    {.type = ErrorType::ImplementationError, .extraInfo = "archive has no roots"}
                );
            if (std::filesystem::exists(options_.localArchivePath) && !options_.mayOverwrite)
                return enterErrorState<WorkStatus>({.type = ErrorType::FileExists});

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
                    options_.progressCallback(0u, totalPayloadBytes_, totalPayloadBytes_, 0);
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

std::expected<void, Operation::Error> ArchiveDownloadOperation::prepareInStrand()
{
    if (options_.createMissingDirectories)
    {
        std::error_code mkErr{};
        std::filesystem::create_directories(tempPath_.parent_path(), mkErr);
        if (mkErr)
        {
            Log::error(
                "ArchiveDownloadOperation: cannot create parents of '{}': {}",
                tempPath_.generic_string(),
                mkErr.message()
            );
            return std::unexpected(Error{.type = ErrorType::CannotCreateDirectory});
        }
    }

    const auto flattenResult = flattenRootsInStrand();
    if (!flattenResult.has_value())
        return std::unexpected(flattenResult.error());

    if (resolvedEntries_.empty())
        return std::unexpected(Error{
            .type = ErrorType::FileNotFound,
            .extraInfo = "no archivable entries under the selected roots",
        });

    auto sinkOrError = openArchiveSink(
        tempPath_, options_.localArchivePath, options_.compression, options_.compressionOptions
    );
    if (!sinkOrError.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: cannot open archive sink: {}",
            sinkOrError.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(sinkOrError.error()));
    }
    writer_ = TarArchive::Writer::makeFromSink(std::move(*sinkOrError));
    return {};
}

std::expected<void, Operation::Error> ArchiveDownloadOperation::flattenRootsInStrand()
{
    for (auto const& root : options_.entries)
    {
        const auto rootFullPath = root.fullPath.empty() ? root.path : root.fullPath;
        const auto basename =
            rootFullPath.has_filename() ? rootFullPath.filename() : rootFullPath;

        if (root.type == SharedData::FileType::Regular)
        {
            ResolvedEntry resolved{};
            resolved.tarMeta = root;
            resolved.tarMeta.path = basename;
            resolved.tarMeta.fullPath = basename;
            resolved.remoteFullPath = rootFullPath;
            totalPayloadBytes_ += resolved.tarMeta.size;
            resolvedEntries_.push_back(std::move(resolved));
        }
        else if (root.type == SharedData::FileType::Directory)
        {
            ResolvedEntry dirEntry{};
            dirEntry.tarMeta = root;
            dirEntry.tarMeta.path = basename;
            dirEntry.tarMeta.fullPath = basename;
            dirEntry.tarMeta.size = 0u;
            resolvedEntries_.push_back(std::move(dirEntry));

            const auto recurseResult = recurseIntoDirectoryInStrand(rootFullPath, basename);
            if (!recurseResult.has_value())
                return std::unexpected(recurseResult.error());
        }
        else
        {
            Log::warn(
                "ArchiveDownloadOperation: skipping non-regular, non-directory root '{}' (type={})",
                rootFullPath.generic_string(),
                static_cast<int>(root.type)
            );
        }
    }
    return {};
}

std::expected<void, Operation::Error> ArchiveDownloadOperation::recurseIntoDirectoryInStrand(
    std::filesystem::path const& remoteFullPath, std::filesystem::path const& archivePrefix
)
{
    auto listResult = sftp_->listDirectoryInStrand(remoteFullPath);
    if (!listResult.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: listDirectory failed for '{}': {}",
            remoteFullPath.generic_string(),
            listResult.error().message
        );
        return std::unexpected(Error{
            .type = ErrorType::SftpError,
            .sftpError = listResult.error(),
            .extraInfo = fmt::format("listing remote directory '{}'", remoteFullPath.generic_string()),
        });
    }

    for (auto const& child : listResult.value())
    {
        const std::string name = child.path.filename().generic_string();
        if (name == "." || name == "..")
            continue;

        const auto childFullRemote = remoteFullPath / child.path;
        const auto childArchivePath = archivePrefix / child.path;

        if (child.type == SharedData::FileType::Regular)
        {
            ResolvedEntry resolved{};
            resolved.tarMeta = child;
            resolved.tarMeta.path = childArchivePath;
            resolved.tarMeta.fullPath = childArchivePath;
            resolved.remoteFullPath = childFullRemote;
            totalPayloadBytes_ += resolved.tarMeta.size;
            resolvedEntries_.push_back(std::move(resolved));
        }
        else if (child.type == SharedData::FileType::Directory)
        {
            ResolvedEntry dirEntry{};
            dirEntry.tarMeta = child;
            dirEntry.tarMeta.path = childArchivePath;
            dirEntry.tarMeta.fullPath = childArchivePath;
            dirEntry.tarMeta.size = 0u;
            resolvedEntries_.push_back(std::move(dirEntry));

            const auto recurseResult =
                recurseIntoDirectoryInStrand(childFullRemote, childArchivePath);
            if (!recurseResult.has_value())
                return std::unexpected(recurseResult.error());
        }
        else
        {
            Log::warn(
                "ArchiveDownloadOperation: skipping non-regular child '{}' (type={})",
                childFullRemote.generic_string(),
                static_cast<int>(child.type)
            );
        }
    }
    return {};
}

std::expected<void, Operation::Error> ArchiveDownloadOperation::openNextEntryInStrand()
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
        auto entryOrError = writer_->beginEntry(resolved.tarMeta);
        if (!entryOrError.has_value())
        {
            Log::error(
                "ArchiveDownloadOperation: cannot begin tar entry '{}': {}",
                resolved.tarMeta.path.generic_string(),
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

    const auto streamOpenResult = sftp_->openFileInStrand(
        resolved.remoteFullPath,
        SecureShell::SftpSession::OpenType::Read,
        std::filesystem::perms::unknown
    );
    if (!streamOpenResult.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: cannot open remote '{}': {}",
            resolved.remoteFullPath.generic_string(),
            streamOpenResult.error().message
        );
        return std::unexpected(Error{
            .type = ErrorType::SftpError,
            .sftpError = streamOpenResult.error(),
            .extraInfo = fmt::format(
                "Opening remote file: {}", resolved.remoteFullPath.generic_string()
            ),
        });
    }
    currentStream_ = streamOpenResult.value();

    auto entryWriterOrError = writer_->beginEntry(resolved.tarMeta);
    if (!entryWriterOrError.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: cannot begin tar entry '{}': {}",
            resolved.tarMeta.path.generic_string(),
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
    const std::uint64_t remaining = resolved.tarMeta.size - currentEntryBytesRead_;
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
            resolved.remoteFullPath.generic_string(),
            readResult.error().message
        );
        return std::unexpected(Error{
            .type = ErrorType::SftpError,
            .sftpError = readResult.error(),
            .extraInfo = fmt::format(
                "Reading remote file: {}", resolved.remoteFullPath.generic_string()
            ),
        });
    }
    const auto bytesRead = readResult.value();
    if (bytesRead == 0)
    {
        Log::warn(
            "ArchiveDownloadOperation: premature EOF on '{}' (expected {} more bytes)",
            resolved.remoteFullPath.generic_string(),
            remaining
        );
        const auto closeResult = finaliseEntryInStrand();
        if (!closeResult.has_value())
            return std::unexpected(closeResult.error());
        return true;
    }

    const auto writeResult = currentEntry_->write(std::span<std::byte const>{
        reinterpret_cast<std::byte const*>(buffer_.data()),
        static_cast<std::size_t>(bytesRead),
    });
    if (!writeResult.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: tar write failure on '{}': {}",
            resolved.tarMeta.path.generic_string(),
            writeResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(writeResult.error()));
    }

    currentEntryBytesRead_ += static_cast<std::uint64_t>(bytesRead);
    totalBytesTransferred_ += static_cast<std::uint64_t>(bytesRead);
    options_.progressCallback(0u, totalPayloadBytes_, totalBytesTransferred_, 0);

    if (currentEntryBytesRead_ >= resolved.tarMeta.size)
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
    if (currentEntryBytesRead_ != resolved.tarMeta.size)
    {
        Log::warn(
            "ArchiveDownloadOperation: entry '{}' truncated ({} of {} bytes) — archive padded with zeros",
            resolved.tarMeta.path.generic_string(),
            currentEntryBytesRead_,
            resolved.tarMeta.size
        );
        ++currentEntryIndex_;
        return {};
    }

    const auto closeResult = std::move(handle).close();
    if (!closeResult.has_value())
    {
        Log::error(
            "ArchiveDownloadOperation: tar entry close failed for '{}': {}",
            resolved.tarMeta.path.generic_string(),
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

    if (std::filesystem::exists(options_.localArchivePath) && !options_.mayOverwrite)
        return std::unexpected(Error{.type = ErrorType::FileExists});

    std::error_code renameErr{};
    std::filesystem::rename(tempPath_, options_.localArchivePath, renameErr);
    if (renameErr)
    {
        Log::error("ArchiveDownloadOperation: rename failed: {}", renameErr.message());
        return std::unexpected(Error{.type = ErrorType::RenameFailure});
    }
    return {};
}

std::expected<void, Operation::Error> ArchiveDownloadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info(
            "ArchiveDownloadOperation: cancelled ({} roots, {} bytes planned).",
            options_.entries.size(),
            totalPayloadBytes_
        );
        state_ = OperationState::Canceled;
    }
    cleanup();
    return {};
}

void ArchiveDownloadOperation::cleanup()
{
    currentEntry_.reset();
    if (auto stream = currentStream_.lock(); stream)
        stream->close(false);
    currentStream_.reset();
    writer_.reset();

    if (!tempPath_.empty() && std::filesystem::exists(tempPath_))
    {
        std::error_code removeErr{};
        std::filesystem::remove(tempPath_, removeErr);
    }
}
