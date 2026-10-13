#include <backend/sftp/bulk_download_operation.hpp>
#include <paths/native.hpp>
#include <constants/sftp.hpp>
#include <log/log.hpp>
#include <utility/permissions.hpp>

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

std::expected<void, BulkDownloadOperation::Error>
BulkDownloadOperation::prepareRootInStrand(SharedData::DirectoryEntry const& root)
{
    // Base directory of the download, everything after this will be relative to this:
    return createLocalDirectory(common().localPath, root);
}

std::expected<void, BulkDownloadOperation::Error>
BulkDownloadOperation::createDirectoryInStrand(std::size_t entryIndex, EntryPaths const& paths)
{
    return createLocalDirectory(paths.local, entries()[entryIndex]);
}

std::expected<void, BulkDownloadOperation::Error>
BulkDownloadOperation::createLocalDirectory(Paths::LocalPath const& localPath, SharedData::DirectoryEntry const& entry)
{
    if (localPath.empty())
        return std::unexpected(Error{.type = ErrorType::ImplementationError, .extraInfo = "No local directory"});
    const auto path = localPath.native();
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
    return applyPermsToDirectory(path, entry);
}

std::unique_ptr<Operation> BulkDownloadOperation::makeChild(
    std::size_t entryIndex,
    EntryPaths const& paths,
    std::function<void(std::uint64_t, std::uint64_t)> onProgress
)
{
    auto const& entry = entries()[entryIndex];

    auto downloadOptions = individualOptions_;
    downloadOptions.remotePath = paths.remote;
    downloadOptions.localPath = paths.local;
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
    // Missing or unreadable single files, and local files that cannot be written, do not compromise
    // the rest of the bulk.
    if (error.type == ErrorType::OpenFailure || error.type == ErrorType::CannotCreateDirectory)
        return true;
    return error.sftpError &&
        (error.sftpError->sftpError == SSH_FX_NO_SUCH_FILE || error.sftpError->sftpError == SSH_FX_PERMISSION_DENIED ||
            error.sftpError->sftpError == SSH_FX_FAILURE);
}

std::expected<void, BulkDownloadOperation::Error> BulkDownloadOperation::applyPermsToDirectory(
    std::filesystem::path const& path,
    SharedData::DirectoryEntry const& entryToInheritFrom
)
{
    // Unknown permissions (the scan root carries none) leave the directory as created.
    std::optional<std::filesystem::perms> toApplyPerms{std::nullopt};
    if (individualOptions_.inheritPermissions)
        toApplyPerms = Utility::inheritablePermissions(entryToInheritFrom.permissions);
    else if (individualOptions_.directoryPermissions)
        toApplyPerms = Utility::sanitizePermissions(*individualOptions_.directoryPermissions);

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
    std::vector<EntryPaths> paths;
    entries.reserve(files.size());
    paths.reserve(files.size());
    std::uint64_t totalBytes = 0;
    for (auto& file : files)
    {
        SharedData::DirectoryEntry entry{};
        // Flat entries carry their name only, the paths come from the prescanned paths.
        if (const auto name = file.source.filename(); name)
            entry.name = name->value();
        entry.type = SharedData::FileType::Regular;
        entry.size = file.sizeBytes;
        entry.mtime = file.mtime;
        entry.mtimeNsec = file.mtimeNsec;
        totalBytes += file.sizeBytes;
        entries.push_back(std::move(entry));
        paths.push_back(
            EntryPaths{
                .remote = std::move(file.source), .local = std::move(file.destination), .mayOverwrite = file.mayOverwrite
            }
        );
    }
    setPrescannedEntries(std::move(entries), std::move(paths), totalBytes);
}
