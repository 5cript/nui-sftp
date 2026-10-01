#include <backend/sftp/bulk_upload_operation.hpp>
#include <constants/sftp.hpp>
#include <log/log.hpp>

namespace
{
    BulkTransferOperation::CommonOptions toCommon(BulkUploadOperation::BulkUploadOperationOptions const& options)
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

BulkUploadOperation::BulkUploadOperation(SecureShell::SftpSession& sftp, BulkUploadOperationOptions options)
    : BulkTransferOperation{sftp, toCommon(options)}
    , individualOptions_{std::move(options.individualOptions)}
{}

BulkUploadOperation::~BulkUploadOperation() = default;

SharedData::OperationType BulkUploadOperation::type() const
{
    return SharedData::OperationType::BulkUpload;
}

std::filesystem::path BulkUploadOperation::localPathOf(std::size_t entryIndex) const
{
    // Prescanned entries carry the absolute local source in entry.path.
    if (prescanned())
        return entries()[entryIndex].path;
    return (common().localPath / SharedData::fullPathRelative(entries(), entries()[entryIndex])).lexically_normal();
}

std::filesystem::path BulkUploadOperation::remotePathOf(std::size_t entryIndex) const
{
    if (prescanned())
        return prescannedDestination(entryIndex);
    return (common().remotePath / SharedData::fullPathRelative(entries(), entries()[entryIndex])).lexically_normal();
}

std::filesystem::path BulkUploadOperation::displayPath(std::size_t entryIndex) const
{
    return remotePathOf(entryIndex);
}

std::expected<void, BulkUploadOperation::Error>
BulkUploadOperation::prepareRootInStrand(SharedData::DirectoryEntry const& root)
{
    // Base directory of the upload, everything after this will be relative to this:
    return createRemoteDirectoryInStrand(common().remotePath, root);
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::createDirectoryInStrand(std::size_t entryIndex)
{
    return createRemoteDirectoryInStrand(remotePathOf(entryIndex), entries()[entryIndex]);
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::createRemoteDirectoryInStrand(
    std::filesystem::path const& path,
    SharedData::DirectoryEntry const& entry
)
{
    const auto result = sftp().createDirectoryIfItDoesntExistInStrand(path, determinePerms(entry));
    if (!result.has_value())
    {
        Log::error("BulkUploadOperation: Failed to create remote sftp directory: {}.", result.error().message);
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = result.error(),
                .extraInfo = fmt::format("Creating remote directory: {}", path.string())
            }
        );
    }
    return {};
}

std::unique_ptr<Operation>
BulkUploadOperation::makeChild(std::size_t entryIndex, std::function<void(std::uint64_t, std::uint64_t)> onProgress)
{
    auto const& entry = entries()[entryIndex];

    auto uploadOptions = individualOptions_;
    uploadOptions.remotePath = remotePathOf(entryIndex);
    uploadOptions.localPath = localPathOf(entryIndex);
    uploadOptions.bigFileOptimized = entry.size >= Constants::bigFileCutOff;
    uploadOptions.progressCallback = [onProgress = std::move(onProgress)](auto, auto max, auto current, auto)
    {
        onProgress(current, max);
    };

    return std::make_unique<UploadOperation>(sftp(), std::move(uploadOptions));
}

bool BulkUploadOperation::isSkippableError(Error const& error) const
{
    // Local read problems (no sftp error attached) and missing or unwritable remote targets do
    // not compromise the rest of the bulk.
    return !error.sftpError ||
        (error.sftpError->sftpError == SSH_FX_NO_SUCH_FILE || error.sftpError->sftpError == SSH_FX_PERMISSION_DENIED ||
            error.sftpError->sftpError == SSH_FX_FAILURE);
}

std::filesystem::perms BulkUploadOperation::determinePerms(SharedData::DirectoryEntry const& entry) const
{
    if (individualOptions_.inheritPermissions)
    {
        auto inherited = entry.permissions;
        if (entry.isRegularFile())
            inherited |= std::filesystem::perms::owner_write;
        return inherited;
    }

    if (entry.isDirectory())
    {
        if (individualOptions_.directoryPermissions)
            return individualOptions_.directoryPermissions.value() | std::filesystem::perms::owner_write;
        return std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec;
    }

    if (individualOptions_.filePermissions)
        return individualOptions_.filePermissions.value() | std::filesystem::perms::owner_write;
    // Inherit execute permissions anyway:
    const auto ownerExecute = entry.permissions & std::filesystem::perms::owner_exec;
    const auto groupExecute = entry.permissions & std::filesystem::perms::group_exec;
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | ownerExecute |
        std::filesystem::perms::group_read | std::filesystem::perms::group_write | groupExecute;
}

void BulkUploadOperation::setPrescannedFileList(std::vector<PrescannedFile> files)
{
    std::vector<SharedData::DirectoryEntry> entries;
    std::vector<std::filesystem::path> destinations;
    entries.reserve(files.size());
    destinations.reserve(files.size());
    std::uint64_t totalBytes = 0;
    for (auto& file : files)
    {
        SharedData::DirectoryEntry entry{};
        entry.path = file.localSrc;
        entry.fullPath = file.localSrc;
        entry.type = SharedData::FileType::Regular;
        entry.size = file.sizeBytes;
        totalBytes += file.sizeBytes;
        entries.push_back(std::move(entry));
        destinations.push_back(std::move(file.remoteDst));
    }
    setPrescannedEntries(std::move(entries), std::move(destinations), totalBytes);
}
