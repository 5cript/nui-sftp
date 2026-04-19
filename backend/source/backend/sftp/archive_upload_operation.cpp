#include <backend/sftp/archive_upload_operation.hpp>

#include <tar_archive/error.hpp>

#include <log/log.hpp>

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
     * @brief Build a DirectoryEntry from a local filesystem path. The @p archivePath
     *        is what goes into the tar header (relative to the archive root); the
     *        payload is read from @p localPath.
     */
    std::optional<SharedData::DirectoryEntry>
    localPathToEntry(
        std::filesystem::path const& localPath,
        std::filesystem::path const& archivePath
    )
    {
        std::error_code statErr{};
        const auto status = std::filesystem::symlink_status(localPath, statErr);
        if (statErr)
        {
            Log::warn(
                "ArchiveUploadOperation: cannot stat '{}': {}",
                localPath.generic_string(),
                statErr.message()
            );
            return std::nullopt;
        }

        SharedData::DirectoryEntry entry{};
        entry.path = archivePath;
        entry.fullPath = archivePath;
        entry.permissions = status.permissions();
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
    return entry.tarMeta.type == SharedData::FileType::Regular;
}

ArchiveUploadOperation::ArchiveUploadOperation(SecureShell::SftpSession& sftp, Options options)
    : Operation{}
    , sftp_{&sftp}
    , options_{std::move(options)}
{}

ArchiveUploadOperation::~ArchiveUploadOperation()
{
    std::ignore = cancel(false);
    if (auto stream = remoteStream_.lock(); stream)
        stream->strand()->pushPromiseTask([]() {}).get();
}

std::expected<Operation::WorkStatus, Operation::Error> ArchiveUploadOperation::work()
{
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

void ArchiveUploadOperation::flattenRoots()
{
    resolvedEntries_.clear();
    resolvedEntries_.reserve(options_.localPaths.size());

    for (auto const& localRoot : options_.localPaths)
    {
        const auto basename =
            localRoot.has_filename() ? localRoot.filename() : localRoot;
        auto rootEntry = localPathToEntry(localRoot, basename);
        if (!rootEntry)
            continue;

        if (rootEntry->type == SharedData::FileType::Regular)
        {
            if (rootEntry->type == SharedData::FileType::Regular)
                totalPayloadBytes_ += rootEntry->size;
            ResolvedEntry resolved{};
            resolved.tarMeta = std::move(*rootEntry);
            resolved.localFullPath = localRoot;
            resolvedEntries_.push_back(std::move(resolved));
        }
        else if (rootEntry->type == SharedData::FileType::Directory)
        {
            rootEntry->size = 0u;
            ResolvedEntry dirEntry{};
            dirEntry.tarMeta = std::move(*rootEntry);
            dirEntry.localFullPath.clear();
            resolvedEntries_.push_back(std::move(dirEntry));

            recurseIntoDirectory(localRoot, basename);
        }
        else
        {
            Log::warn(
                "ArchiveUploadOperation: skipping non-regular, non-directory root '{}' (type={})",
                localRoot.generic_string(),
                static_cast<int>(rootEntry->type)
            );
        }
    }
}

void ArchiveUploadOperation::recurseIntoDirectory(
    std::filesystem::path const& localRoot, std::filesystem::path const& archivePrefix
)
{
    std::error_code iterErr{};
    std::filesystem::recursive_directory_iterator walker{
        localRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iterErr,
    };
    if (iterErr)
    {
        Log::warn(
            "ArchiveUploadOperation: cannot enumerate '{}': {}",
            localRoot.generic_string(),
            iterErr.message()
        );
        return;
    }

    const std::filesystem::recursive_directory_iterator end{};
    while (walker != end)
    {
        std::error_code advanceErr{};
        auto const& dirEntry = *walker;

        const auto relativePath =
            std::filesystem::relative(dirEntry.path(), localRoot, advanceErr);
        if (advanceErr)
        {
            walker.increment(advanceErr);
            continue;
        }
        const auto archivePath = archivePrefix / relativePath;

        auto resolvedMeta = localPathToEntry(dirEntry.path(), archivePath);
        if (resolvedMeta)
        {
            if (resolvedMeta->type == SharedData::FileType::Regular)
            {
                totalPayloadBytes_ += resolvedMeta->size;
                ResolvedEntry resolved{};
                resolved.tarMeta = std::move(*resolvedMeta);
                resolved.localFullPath = dirEntry.path();
                resolvedEntries_.push_back(std::move(resolved));
            }
            else if (resolvedMeta->type == SharedData::FileType::Directory)
            {
                resolvedMeta->size = 0u;
                ResolvedEntry resolvedDir{};
                resolvedDir.tarMeta = std::move(*resolvedMeta);
                resolvedDir.localFullPath.clear();
                resolvedEntries_.push_back(std::move(resolvedDir));
            }
            else
            {
                Log::warn(
                    "ArchiveUploadOperation: skipping non-regular child '{}' (type={})",
                    dirEntry.path().generic_string(),
                    static_cast<int>(resolvedMeta->type)
                );
            }
        }
        walker.increment(advanceErr);
    }
}

std::expected<void, Operation::Error> ArchiveUploadOperation::prepareInStrand()
{
    flattenRoots();

    if (resolvedEntries_.empty())
        return std::unexpected(Error{
            .type = ErrorType::FileNotFound,
            .extraInfo = "no local files could be stat'd",
        });

    using OpenType = SecureShell::SftpSession::OpenType;
    OpenType openType = OpenType::Write | OpenType::Create | OpenType::Truncate;
    if (!options_.mayOverwrite)
        openType = OpenType::Write | OpenType::Create | OpenType::Exclusive;

    const auto openResult = sftp_->openFileInStrand(
        options_.remoteArchivePath, openType, std::filesystem::perms{0644u}
    );
    if (!openResult.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: cannot open remote '{}': {}",
            options_.remoteArchivePath.generic_string(),
            openResult.error().message
        );
        return std::unexpected(Error{
            .type = ErrorType::SftpError,
            .sftpError = openResult.error(),
            .extraInfo = fmt::format(
                "Opening remote archive file: {}", options_.remoteArchivePath.generic_string()
            ),
        });
    }
    remoteStream_ = openResult.value();

    writer_ = TarArchive::Writer::makeFromSink(
        std::make_unique<SftpFileByteSink>(remoteStream_)
    );
    return {};
}

std::expected<void, Operation::Error> ArchiveUploadOperation::openNextEntryInStrand()
{
    auto& resolved = resolvedEntries_[currentEntryIndex_];

    if (!isPayloadEntry(resolved))
    {
        // Directory entry — emit the tar header only and advance.
        auto entryOrError = writer_->beginEntry(resolved.tarMeta);
        if (!entryOrError.has_value())
        {
            Log::error(
                "ArchiveUploadOperation: cannot begin tar entry '{}': {}",
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

    currentLocalFile_.open(resolved.localFullPath, std::ios::binary);
    if (!currentLocalFile_.is_open())
    {
        Log::error(
            "ArchiveUploadOperation: cannot open local '{}'",
            resolved.localFullPath.generic_string()
        );
        return std::unexpected(Error{
            .type = ErrorType::OpenFailure,
            .extraInfo = resolved.localFullPath.generic_string(),
        });
    }

    auto entryWriterOrError = writer_->beginEntry(resolved.tarMeta);
    if (!entryWriterOrError.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: cannot begin tar entry '{}': {}",
            resolved.tarMeta.path.generic_string(),
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
    currentLocalFile_.read(buffer_.data(), static_cast<std::streamsize>(requestBytes));
    const auto bytesRead = static_cast<std::size_t>(currentLocalFile_.gcount());
    if (bytesRead == 0u)
    {
        Log::warn(
            "ArchiveUploadOperation: local '{}' truncated mid-read (expected {} more bytes)",
            resolved.localFullPath.generic_string(),
            remaining
        );
        const auto closeResult = finaliseEntryInStrand();
        if (!closeResult.has_value())
            return std::unexpected(closeResult.error());
        return true;
    }

    const auto writeResult = currentEntry_->write(std::span<std::byte const>{
        reinterpret_cast<std::byte const*>(buffer_.data()), bytesRead,
    });
    if (!writeResult.has_value())
    {
        Log::error(
            "ArchiveUploadOperation: tar write failure on '{}': {}",
            resolved.tarMeta.path.generic_string(),
            writeResult.error().toString()
        );
        return std::unexpected(archiveErrorToOperationError(writeResult.error()));
    }

    currentEntryBytesRead_ += bytesRead;
    totalBytesTransferred_ += bytesRead;
    options_.progressCallback(0u, totalPayloadBytes_, totalBytesTransferred_, 0);

    if (currentEntryBytesRead_ >= resolved.tarMeta.size)
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
    if (currentEntryBytesRead_ != resolved.tarMeta.size)
    {
        Log::warn(
            "ArchiveUploadOperation: entry '{}' truncated ({} of {} bytes) — archive padded with zeros",
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
            "ArchiveUploadOperation: tar entry close failed for '{}': {}",
            resolved.tarMeta.path.generic_string(),
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
    return {};
}

std::expected<void, Operation::Error> ArchiveUploadOperation::cancel(bool adoptCancelState)
{
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

void ArchiveUploadOperation::cleanup()
{
    currentEntry_.reset();
    currentLocalFile_.close();
    writer_.reset();
    if (auto stream = remoteStream_.lock(); stream)
        stream->close(false);
    remoteStream_.reset();
}
