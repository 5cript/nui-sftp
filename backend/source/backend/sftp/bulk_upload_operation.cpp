#include <backend/sftp/bulk_upload_operation.hpp>
#include <backend/sftp/unsafe_fast_mode.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <log/log.hpp>
#include <remote_io/remote_file_system_helpers.hpp>
#include <utility/checked_symlink_status.hpp>
#include <utility/permissions.hpp>

#include <filesystem>
#include <ranges>
#include <system_error>
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
            .failureToast = options.individualOptions.failureToast,
        };
    }
}

BulkUploadOperation::BulkUploadOperation(
    std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem,
    BulkUploadOperationOptions options
)
    : BulkTransferOperation{std::move(fileSystem), toCommon(options)}
    , individualOptions_{std::move(options.individualOptions)}
{}

BulkUploadOperation::~BulkUploadOperation() = default;

SharedData::OperationType BulkUploadOperation::type() const
{
    return SharedData::OperationType::BulkUpload;
}

std::expected<BulkUploadOperation::WorkStatus, BulkUploadOperation::Error> BulkUploadOperation::work()
{
    if (individualOptions_.sourceMayVanish && scanning())
    {
        // A root that cannot be checked is not gone, its scan fails.
        if (const auto status = Utility::checkedSymlinkStatus(common().localPath.native());
            status && !status->has_value())
        {
            Log::info("BulkUploadOperation: '{}' is gone, nothing to upload.", common().localPath.toUtf8());
            // Ends without a transfer, so its owner does not take the destination for uploaded.
            enterState(OperationState::Canceled);
            return WorkStatus::Complete;
        }
    }
    return BulkTransferOperation::work();
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::prepareRootInStrand(
    RemoteIo::IRemoteFileSystem& fileSystem,
    SharedData::DirectoryEntry const& root
)
{
    // Refused once for the whole tree rather than for each of its files.
    if (uploadPathOf(individualOptions_.unsafeFastMode, 0u, fileSystem.capabilities().probeEntries) ==
        UploadPath::Refused)
        return std::unexpected(
            UploadOperation::uncheckableDestination(common().remotePath, individualOptions_.failureToast)
        );
    // Base directory of the upload, everything after this will be relative to this:
    return createRemoteDirectoryInStrand(fileSystem, common().remotePath, root);
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::createDirectoryInStrand(
    RemoteIo::IRemoteFileSystem& fileSystem,
    std::size_t entryIndex,
    EntryPaths const& paths
)
{
    // A scanned directory comes after its parent, a prescanned one may be the first entry below it.
    if (prescanned() && individualOptions_.createMissingDirectories)
    {
        if (auto parents = createMissingParentsInStrand(fileSystem, paths.remote); !parents)
            return parents;
    }
    return createRemoteDirectoryInStrand(fileSystem, paths.remote, entries()[entryIndex]);
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::createMissingParentsInStrand(
    RemoteIo::IRemoteFileSystem& fileSystem,
    Paths::RemotePath const& path
)
{
    auto const& root = common().remotePath;
    std::vector<Paths::RemotePath> missing;
    for (auto parent = path.parent(); parent && *parent != root && parent->isWithin(root); parent = parent->parent())
    {
        const auto stat = RemoteIo::probeEntryInStrand(fileSystem, *parent);
        if (stat)
            break;
        if (stat.error().kind != RemoteIo::RemoteErrorKind::NoSuchFile)
        {
            return std::unexpected(
                Error{
                    .type = ErrorType::SftpError,
                    .remoteError = stat.error(),
                    .extraInfo = fmt::format("Inspecting remote directory: {}", parent->value())
                }
            );
        }
        missing.push_back(*parent);
    }
    const SharedData::DirectoryEntry directory{.type = SharedData::FileType::Directory};
    for (auto const& parent : missing | std::views::reverse)
    {
        if (auto created = createRemoteDirectoryInStrand(fileSystem, parent, directory); !created)
            return created;
    }
    return {};
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::createRemoteDirectoryInStrand(
    RemoteIo::IRemoteFileSystem& fileSystem,
    Paths::RemotePath const& path,
    SharedData::DirectoryEntry const& entry
)
{
    const auto result = fileSystem.inStrand(
        RemoteIo::CreateDirectoryIfItDoesntExist{.path = path, .permissions = determinePerms(entry)}
    );
    if (!result.has_value())
    {
        Log::error(
            individualOptions_.failureToast,
            "BulkUploadOperation: Failed to create remote sftp directory: {}.",
            result.error().message
        );
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .remoteError = result.error(),
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
    uploadOptions.bigFileOptimized = childTakesAsynchronousPath(entry.size);
    uploadOptions.progressCallback = [onProgress = std::move(onProgress)](auto, auto max, auto current, auto)
    {
        onProgress(current, max);
    };

    return std::make_unique<UploadOperation>(fileSystem(), std::move(uploadOptions));
}

bool BulkUploadOperation::isSkippableError(Error const& error) const
{
    // Local read problems (no remote error attached) and missing or unwritable remote targets do
    // not compromise the rest of the bulk.
    using RemoteIo::RemoteErrorKind;
    return !error.remoteError ||
        (error.remoteError->kind == RemoteErrorKind::NoSuchFile ||
            error.remoteError->kind == RemoteErrorKind::PermissionDenied ||
            error.remoteError->kind == RemoteErrorKind::Failure);
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

std::optional<Operation::KeptPartialFiles> BulkUploadOperation::keptPartialFiles() const
{
    if (!keepsPartialFiles())
        return std::nullopt;
    const auto suffix = Persistence::effectiveTempFileSuffix(individualOptions_.tempFileSuffix);
    KeptPartialFiles kept{};
    for (const auto index : unfinishedEntryIndices())
    {
        if (!entries()[index].isRegularFile())
            continue;
        const auto paths = entryPaths(index);
        if (!paths)
            continue;
        if (auto temporary = paths->remote.withFilenameSuffix(suffix); temporary)
            kept.remote.push_back(*std::move(temporary));
    }
    return kept;
}
