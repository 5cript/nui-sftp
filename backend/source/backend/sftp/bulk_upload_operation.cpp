#include <backend/sftp/bulk_upload_operation.hpp>
#include <paths/native.hpp>
#include <constants/sftp.hpp>
#include <log/log.hpp>
#include <utility/permissions.hpp>

#include <ranges>
#include <vector>

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

BulkUploadOperation::BulkUploadOperation(
    std::weak_ptr<SecureShell::SftpSession> sftp,
    BulkUploadOperationOptions options
)
    : BulkTransferOperation{std::move(sftp), toCommon(options)}
    , individualOptions_{std::move(options.individualOptions)}
{}

BulkUploadOperation::~BulkUploadOperation() = default;

SharedData::OperationType BulkUploadOperation::type() const
{
    return SharedData::OperationType::BulkUpload;
}

std::expected<void, BulkUploadOperation::Error>
BulkUploadOperation::prepareRootInStrand(SecureShell::SftpSession& sftp, SharedData::DirectoryEntry const& root)
{
    // Base directory of the upload, everything after this will be relative to this:
    return createRemoteDirectoryInStrand(sftp, common().remotePath, root);
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::createDirectoryInStrand(
    SecureShell::SftpSession& sftp,
    std::size_t entryIndex,
    EntryPaths const& paths
)
{
    // A scanned directory comes after its parent, a prescanned one may be the first entry below it.
    if (prescanned() && individualOptions_.createMissingDirectories)
    {
        if (auto parents = createMissingParentsInStrand(sftp, paths.remote); !parents)
            return parents;
    }
    return createRemoteDirectoryInStrand(sftp, paths.remote, entries()[entryIndex]);
}

std::expected<void, BulkUploadOperation::Error>
BulkUploadOperation::createMissingParentsInStrand(SecureShell::SftpSession& sftp, Paths::RemotePath const& path)
{
    auto const& root = common().remotePath;
    std::vector<Paths::RemotePath> missing;
    for (auto parent = path.parent(); parent && *parent != root && parent->isWithin(root); parent = parent->parent())
    {
        const auto stat = sftp.lstatInStrand(*parent);
        if (stat)
            break;
        if (stat.error().sftpError != SSH_FX_NO_SUCH_FILE)
        {
            return std::unexpected(
                Error{
                    .type = ErrorType::SftpError,
                    .sftpError = stat.error(),
                    .extraInfo = fmt::format("Inspecting remote directory: {}", parent->value())
                }
            );
        }
        missing.push_back(*parent);
    }
    const SharedData::DirectoryEntry directory{.type = SharedData::FileType::Directory};
    for (auto const& parent : missing | std::views::reverse)
    {
        if (auto created = createRemoteDirectoryInStrand(sftp, parent, directory); !created)
            return created;
    }
    return {};
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::createRemoteDirectoryInStrand(
    SecureShell::SftpSession& sftp,
    Paths::RemotePath const& path,
    SharedData::DirectoryEntry const& entry
)
{
    const auto result = sftp.createDirectoryIfItDoesntExistInStrand(path, determinePerms(entry));
    if (!result.has_value())
    {
        Log::error("BulkUploadOperation: Failed to create remote sftp directory: {}.", result.error().message);
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = result.error(),
                .extraInfo = fmt::format("Creating remote directory: {}", path.value())
            }
        );
    }
    return {};
}

std::unique_ptr<Operation> BulkUploadOperation::makeChild(
    std::size_t entryIndex,
    EntryPaths const& paths,
    std::function<void(std::uint64_t, std::uint64_t)> onProgress
)
{
    auto const& entry = entries()[entryIndex];

    auto uploadOptions = individualOptions_;
    uploadOptions.remotePath = paths.remote;
    uploadOptions.localPath = paths.local;
    uploadOptions.mayOverwrite = uploadOptions.mayOverwrite || paths.mayOverwrite;
    uploadOptions.comparedDestination = paths.comparedDestination;
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
    // Unknown permissions (the scan root carries none) fall back to the defaults below.
    if (individualOptions_.inheritPermissions)
    {
        if (auto inherited = Utility::sanitizePermissions(entry.permissions); inherited)
        {
            if (entry.isRegularFile())
                *inherited |= std::filesystem::perms::owner_write;
            return *inherited;
        }
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
    const auto known = Utility::sanitizePermissions(entry.permissions).value_or(std::filesystem::perms::none);
    const auto ownerExecute = known & std::filesystem::perms::owner_exec;
    const auto groupExecute = known & std::filesystem::perms::group_exec;
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | ownerExecute |
        std::filesystem::perms::group_read | std::filesystem::perms::group_write | groupExecute;
}

void BulkUploadOperation::setPrescannedFileList(std::vector<PrescannedFile> files)
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
        entry.type = file.isDirectory ? SharedData::FileType::Directory : SharedData::FileType::Regular;
        entry.size = file.isDirectory ? 0u : file.sizeBytes;
        totalBytes += entry.size;
        entries.push_back(std::move(entry));
        paths.push_back(
            EntryPaths{
                .remote = std::move(file.destination),
                .local = std::move(file.source),
                .mayOverwrite = file.mayOverwrite,
                .comparedDestination = std::move(file.comparedDestination),
            }
        );
    }
    setPrescannedEntries(std::move(entries), std::move(paths), totalBytes);
}

std::optional<SharedData::BulkUploadRequest>
BulkUploadOperation::resumeRequest(SharedData::BulkUploadRequest request) const
{
    if (!hasEntries())
        return std::nullopt;

    request.entries.clear();
    for (const auto index : unfinishedEntryIndices())
    {
        auto const& entry = entries()[index];
        const auto paths = entryPaths(index);
        if (!paths || (!entry.isRegularFile() && !entry.isSymlink() && !entry.isDirectory()))
            continue;
        request.entries.push_back({
            .source = paths->local,
            .destination = paths->remote,
            .sizeBytes = entry.isDirectory() ? 0u : entry.size,
            .isDirectory = entry.isDirectory(),
            .withoutContent = entry.isDirectory(),
            .mayOverwrite = paths->mayOverwrite,
            .comparedDestination = paths->comparedDestination,
        });
    }
    request.remoteRoot = common().remotePath;
    request.localRoot = common().localPath;
    return request;
}
