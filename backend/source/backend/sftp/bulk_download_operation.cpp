#include <backend/sftp/bulk_download_operation.hpp>
#include <constants/sftp.hpp>
#include <log/log.hpp>

namespace
{
    BulkTransferOperation::CommonOptions toCommon(BulkDownloadOperation::BulkDownloadOperationOptions const& options)
    {
        return BulkTransferOperation::CommonOptions{
            .overallProgressCallback = options.overallProgressCallback,
            .remotePath = options.remotePath,
            .localPath = options.localPath,
            .failFast = options.failFast,
            .concurrency = options.concurrency,
            .futureTimeout = options.individualOptions.futureTimeout,
            .progressEmitInterval = options.progressEmitInterval,
        };
    }
}

BulkDownloadOperation::BulkDownloadOperation(SecureShell::SftpSession& sftp, BulkDownloadOperationOptions options)
    : BulkTransferOperation{sftp, toCommon(options)}
    , individualOptions_{std::move(options.individualOptions)}
{}

BulkDownloadOperation::~BulkDownloadOperation() = default;

SharedData::OperationType BulkDownloadOperation::type() const
{
    return SharedData::OperationType::BulkDownload;
}

std::filesystem::path BulkDownloadOperation::localPathOf(std::size_t entryIndex) const
{
    if (prescanned())
        return prescannedDestination(entryIndex);
    return (common().localPath / SharedData::fullPathRelative(entries(), entries()[entryIndex])).lexically_normal();
}

std::filesystem::path BulkDownloadOperation::displayPath(std::size_t entryIndex) const
{
    return SharedData::fullPath(entries(), entries()[entryIndex]);
}

std::expected<void, BulkDownloadOperation::Error>
BulkDownloadOperation::prepareRootInStrand(SharedData::DirectoryEntry const& root)
{
    // Base directory of the download, everything after this will be relative to this:
    const auto path = common().localPath;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        Log::error("BulkDownloadOperation: Failed to create local directory: {}: {}", path.string(), ec.message());
        return std::unexpected(
            Error{
                .type = ErrorType::CannotCreateDirectory,
                .extraInfo = fmt::format("Creating local directory: {}: {}", path.string(), ec.message())
            }
        );
    }
    return applyPermsToDirectory(path, root);
}

std::expected<void, BulkDownloadOperation::Error> BulkDownloadOperation::createDirectoryInStrand(std::size_t entryIndex)
{
    const auto path = localPathOf(entryIndex);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        Log::error("BulkDownloadOperation: Failed to create local directory: {}: {}", path.string(), ec.message());
        return std::unexpected(
            Error{
                .type = ErrorType::CannotCreateDirectory,
                .extraInfo = fmt::format("Creating local directory: {}: {}", path.string(), ec.message())
            }
        );
    }
    return applyPermsToDirectory(path, entries()[entryIndex]);
}

std::unique_ptr<Operation>
BulkDownloadOperation::makeChild(std::size_t entryIndex, std::function<void(std::uint64_t, std::uint64_t)> onProgress)
{
    auto const& entry = entries()[entryIndex];

    auto downloadOptions = individualOptions_;
    downloadOptions.remotePath = displayPath(entryIndex);
    downloadOptions.localPath = localPathOf(entryIndex);
    downloadOptions.bigFileOptimized = entry.size >= Constants::bigFileCutOff;
    downloadOptions.progressCallback = [onProgress = std::move(onProgress)](auto, auto max, auto current, auto)
    {
        onProgress(current, max);
    };
    downloadOptions.entry = entry;

    return std::make_unique<DownloadOperation>(sftp(), std::move(downloadOptions));
}

bool BulkDownloadOperation::isSkippableError(Error const& error) const
{
    // Missing or unreadable single files do not compromise the rest of the bulk.
    return error.sftpError &&
        (error.sftpError->sftpError == SSH_FX_NO_SUCH_FILE || error.sftpError->sftpError == SSH_FX_PERMISSION_DENIED ||
            error.sftpError->sftpError == SSH_FX_FAILURE);
}

std::expected<void, BulkDownloadOperation::Error> BulkDownloadOperation::applyPermsToDirectory(
    std::filesystem::path const& path,
    SharedData::DirectoryEntry const& entryToInheritFrom
)
{
    std::optional<std::filesystem::perms> toApplyPerms{std::nullopt};
    if (individualOptions_.inheritPermissions)
        toApplyPerms = entryToInheritFrom.permissions;
    else if (individualOptions_.directoryPermissions)
        toApplyPerms = individualOptions_.directoryPermissions;

    if (!toApplyPerms)
        return {};

    std::error_code ec;
    std::filesystem::permissions(path, *toApplyPerms, ec);
    if (ec)
    {
        Log::error(
            "BulkDownloadOperation: Failed to set permissions on local directory: {}: {}", path.string(), ec.message()
        );
        return std::unexpected(
            Error{
                .type = ErrorType::CannotSetFilePermissions,
                .extraInfo = fmt::format("Setting permissions on local directory: {}: {}", path.string(), ec.message())
            }
        );
    }
    return {};
}

void BulkDownloadOperation::setPrescannedFileList(std::vector<PrescannedFile> files)
{
    std::vector<SharedData::DirectoryEntry> entries;
    std::vector<std::filesystem::path> destinations;
    entries.reserve(files.size());
    destinations.reserve(files.size());
    std::uint64_t totalBytes = 0;
    for (auto& file : files)
    {
        SharedData::DirectoryEntry entry{};
        // entry.path carries the absolute remote source so SharedData::fullPath returns it
        // verbatim (flat entry, no parent). The local destination lives in the aligned vector.
        entry.path = file.remoteSrc;
        entry.fullPath = file.remoteSrc;
        entry.type = SharedData::FileType::Regular;
        entry.size = file.sizeBytes;
        entry.mtime = file.mtime;
        entry.mtimeNsec = file.mtimeNsec;
        totalBytes += file.sizeBytes;
        entries.push_back(std::move(entry));
        destinations.push_back(std::move(file.localDst));
    }
    setPrescannedEntries(std::move(entries), std::move(destinations), totalBytes);
}
