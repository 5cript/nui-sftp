#include <backend/sftp/bulk_download_operation.hpp>
#include <utility/error_message.hpp>
#include <paths/native.hpp>
#include <constants/sftp.hpp>
#include <log/log.hpp>
#include <utility/link_safe_remove.hpp>
#include <utility/permissions.hpp>

#include <algorithm>
#include <ranges>

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

BulkDownloadOperation::BulkDownloadOperation(
    std::weak_ptr<SecureShell::SftpSession> sftp,
    BulkDownloadOperationOptions options
)
    : BulkTransferOperation{std::move(sftp), toCommon(options)}
    , individualOptions_{std::move(options.individualOptions)}
{
    individualOptions_.verifiedDirectories = std::make_shared<Utility::VerifiedDirectories>();
}

BulkDownloadOperation::~BulkDownloadOperation() = default;

SharedData::OperationType BulkDownloadOperation::type() const
{
    return SharedData::OperationType::BulkDownload;
}

std::expected<void, BulkDownloadOperation::Error>
BulkDownloadOperation::prepareRootInStrand(SecureShell::SftpSession&, SharedData::DirectoryEntry const& root)
{
    // Base directory of the download, everything after this will be relative to this:
    return createLocalDirectory(common().localPath, root);
}

std::expected<void, BulkDownloadOperation::Error> BulkDownloadOperation::createDirectoryInStrand(
    SecureShell::SftpSession&,
    std::size_t entryIndex,
    EntryPaths const& paths
)
{
    // create_directories follows a link planted inside the tree and would create and fill directories outside it.
    std::error_code error{};
    const auto link = Utility::firstLinkBelow(
        common().localPath.native(), paths.local.native(), error, individualOptions_.verifiedDirectories.get()
    );
    if (error)
    {
        return std::unexpected(
            Error{
                .type = ErrorType::FileStatFailed,
                .extraInfo = fmt::format(
                    "Inspecting local directory: {}: {}", paths.local.toUtf8(), Utility::errorMessage(error)
                )
            }
        );
    }
    if (link)
    {
        Log::error("BulkDownloadOperation: '{}' runs through the link '{}'.", paths.local.toUtf8(), Paths::nativeToUtf8(*link));
        return std::unexpected(Error{
            .type = ErrorType::LinkInLocalPath,
            .extraInfo = fmt::format("The local path runs through a link: {}", Paths::nativeToUtf8(*link))
        });
    }
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
        Log::error(
            "BulkDownloadOperation: Failed to create local directory: {}: {}",
            Paths::nativeToUtf8(path),
            Utility::errorMessage(ec)
        );
        return std::unexpected(
            Error{
                .type = ErrorType::CannotCreateDirectory,
                .extraInfo = fmt::format(
                    "Creating local directory: {}: {}", Paths::nativeToUtf8(path), Utility::errorMessage(ec)
                )
            }
        );
    }
    deferDirectoryPermissions(path, entry);
    return {};
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
    downloadOptions.mayOverwrite = downloadOptions.mayOverwrite || paths.mayOverwrite;
    downloadOptions.comparedDestination = paths.comparedDestination;
    downloadOptions.bigFileOptimized = entry.size >= Constants::bigFileCutOff;
    downloadOptions.progressCallback = [onProgress = std::move(onProgress)](auto, auto max, auto current, auto)
    {
        onProgress(current, max);
    };
    downloadOptions.entry = entry;
    downloadOptions.linkFreeRoot = common().localPath;

    return std::make_unique<DownloadOperation>(sftp(), std::move(downloadOptions));
}

bool BulkDownloadOperation::isSkippableError(Error const& error) const
{
    // Missing or unreadable single files, and local files that cannot be written, do not compromise
    // the rest of the bulk.
    // A destination that appeared after the existence check and may not be replaced, or one that changed after a
    // sync compared it, fails only its own entry.
    if (error.type == ErrorType::OpenFailure || error.type == ErrorType::CannotCreateDirectory ||
        error.type == ErrorType::LinkInLocalPath || error.type == ErrorType::FileExists ||
        error.type == ErrorType::ChangedSinceCompare)
        return true;
    return error.sftpError &&
        (error.sftpError->sftpError == SSH_FX_NO_SUCH_FILE || error.sftpError->sftpError == SSH_FX_PERMISSION_DENIED ||
            error.sftpError->sftpError == SSH_FX_FAILURE);
}

bool BulkDownloadOperation::isSkippableDirectoryError(Error const& error) const
{
    return error.type == ErrorType::LinkInLocalPath;
}

void BulkDownloadOperation::deferDirectoryPermissions(
    std::filesystem::path const& path,
    SharedData::DirectoryEntry const& entryToInheritFrom
)
{
    // Unknown permissions (the scan root carries none) leave the directory as created.
    const auto permissions = individualOptions_.inheritPermissions
        ? Utility::inheritablePermissions(entryToInheritFrom.permissions)
        : individualOptions_.directoryPermissions.and_then(Utility::sanitizePermissions);
    if (permissions)
        deferredDirectoryPermissions_.emplace_back(path, *permissions);
}

std::expected<void, BulkDownloadOperation::Error> BulkDownloadOperation::finishInStrand()
{
    // Children first, a parent that loses its search permission would stop them.
    for (auto const& [path, permissions] : deferredDirectoryPermissions_ | std::views::reverse)
    {
        std::error_code error{};
        std::filesystem::permissions(path, permissions, error);
        if (error)
        {
            Log::error(
                "BulkDownloadOperation: Failed to set permissions on local directory: {}: {}",
                Paths::nativeToUtf8(path),
                Utility::errorMessage(error)
            );
            return std::unexpected(
                Error{
                    .type = ErrorType::CannotSetFilePermissions,
                    .extraInfo = fmt::format(
                        "Setting permissions on local directory: {}: {}",
                        Paths::nativeToUtf8(path),
                        Utility::errorMessage(error)
                    ),
                }
            );
        }
    }
    deferredDirectoryPermissions_.clear();
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
        entry.type = file.isDirectory ? SharedData::FileType::Directory : SharedData::FileType::Regular;
        entry.size = file.isDirectory ? 0u : file.sizeBytes;
        entry.mtime = file.mtime;
        entry.mtimeNsec = file.mtimeNsec;
        totalBytes += entry.size;
        entries.push_back(std::move(entry));
        paths.push_back(
            EntryPaths{
                .remote = std::move(file.source),
                .local = std::move(file.destination),
                .mayOverwrite = file.mayOverwrite,
                .comparedDestination = std::move(file.comparedDestination),
            }
        );
    }
    setPrescannedEntries(std::move(entries), std::move(paths), totalBytes);
}

std::optional<SharedData::BulkDownloadRequest>
BulkDownloadOperation::resumeRequest(SharedData::BulkDownloadRequest request) const
{
    if (!hasEntries())
        return std::nullopt;
    const auto unfinished = unfinishedEntryIndices();
    const auto isSymlink = [this](std::size_t index)
    {
        return entries()[index].isSymlink();
    };
    if (std::ranges::any_of(unfinished, isSymlink))
        return std::nullopt;

    request.entries.clear();
    for (const auto index : unfinished)
    {
        auto const& entry = entries()[index];
        const auto paths = entryPaths(index);
        if (!paths || (!entry.isRegularFile() && !entry.isDirectory()))
            continue;
        request.entries.push_back({
            .source = paths->remote,
            .destination = paths->local,
            .sizeBytes = entry.isDirectory() ? 0u : entry.size,
            .isDirectory = entry.isDirectory(),
            .withoutContent = entry.isDirectory(),
            .mtime = entry.mtime,
            .mtimeNsec = entry.mtimeNsec,
            .mayOverwrite = paths->mayOverwrite,
            .comparedDestination = paths->comparedDestination,
        });
    }
    request.remoteRoot = common().remotePath;
    request.localRoot = common().localPath;
    return request;
}

void BulkDownloadOperation::keepPartialFiles()
{
    individualOptions_.doCleanup = false;
    BulkTransferOperation::keepPartialFiles();
}

std::optional<Operation::KeptPartialFiles> BulkDownloadOperation::keptPartialFiles() const
{
    if (individualOptions_.doCleanup)
        return std::nullopt;
    const auto suffix = Persistence::effectiveTempFileSuffix(individualOptions_.tempFileSuffix);
    KeptPartialFiles kept{};
    for (const auto index : unfinishedEntryIndices())
    {
        if (!entries()[index].isRegularFile())
            continue;
        if (const auto paths = entryPaths(index); paths)
        {
            auto temporary = paths->local.native();
            temporary += suffix;
            kept.local.push_back(std::move(temporary));
        }
    }
    return kept;
}
