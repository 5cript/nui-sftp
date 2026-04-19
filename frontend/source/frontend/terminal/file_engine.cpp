#include <frontend/terminal/file_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <log/log.hpp>
#include <constants/sftp.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/api/json.hpp>

struct FileEngine::Implementation
{
    bool wasDisposed = false;
    SshTerminalEngine* engine;
    std::optional<Ids::ChannelId> sftpChannelId{std::nullopt};

    Implementation(SshTerminalEngine* engine)
        : engine{engine}
    {}
};

FileEngine::FileEngine(SshTerminalEngine* engine)
    : impl_{std::make_unique<Implementation>(engine)}
{}
FileEngine::~FileEngine() = default;

std::optional<Ids::ChannelId> FileEngine::release()
{
    return std::move(impl_->sftpChannelId);
}

void FileEngine::dispose(std::function<void()> onComplete)
{
    if (!impl_->wasDisposed)
    {
        if (impl_->sftpChannelId)
        {
            Log::info("Closing sftp channel");
            impl_->engine->closeChannel(impl_->sftpChannelId.value(), std::move(onComplete));
        }
    }
    impl_->wasDisposed = true;
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(FileEngine);

void FileEngine::lazyOpen(
    std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> const& onOpen
)
{
    if (impl_->sftpChannelId)
    {
        onOpen(impl_->sftpChannelId, "Channel already open");
        return;
    }

    Log::info("Creating sftp channel");
    impl_->engine->createSftpChannel(
        [this, onOpen](auto const& id, std::string const& info)
        {
            impl_->sftpChannelId = id;
            onOpen(id, info);
        }
    );
}

void FileEngine::listDirectory(
    std::filesystem::path const& path,
    std::function<void(std::optional<std::vector<SharedData::DirectoryEntry>> const&, std::string const& info)>
        onComplete
)
{
    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot list directory, no sftp channel");
                onComplete(std::nullopt, info);
                return;
            }

            Log::info("Listing directory: {}", path.generic_string());
            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::listDirectory", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Log::info("Received response for listing directory.");

                    if (val.hasOwnProperty("error") || !val.hasOwnProperty("entries"))
                    {
                        if (val.hasOwnProperty("error"))
                        {
                            Log::error("(Frontend) Failed to list directory: {}", val["error"].as<std::string>());
                        }
                        else
                        {
                            Log::error("(Frontend) Failed to list directory: no entries");
                        }
                        onComplete(
                            std::nullopt, val.hasOwnProperty("error") ? val["error"].as<std::string>() : "No entries"
                        );
                        return;
                    }

                    onComplete(nlohmann::json::parse(Nui::JSON::stringify(val))["entries"], "Success");
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}

void FileEngine::createDirectory(
    std::filesystem::path const& path,
    std::function<void(bool, std::string const& info)> onComplete
)
{
    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot create directory, no channel");
                onComplete(false, info);
                return;
            }

            Log::info("Creating directory: {}", path.generic_string());
            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::createDirectory", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to create directory: {}", val["error"].as<std::string>());
                        onComplete(false, val["error"].as<std::string>());
                        return;
                    }

                    onComplete(true, "Success");
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}

void FileEngine::createFile(
    std::filesystem::path const& path,
    std::function<void(bool, std::string const& info)> onComplete
)
{
    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot create file, no channel");
                onComplete(false, info);
                return;
            }

            Log::info("Creating file: {}", path.generic_string());
            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::createFile", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to create file: {}", val["error"].as<std::string>());
                        onComplete(false, val["error"].as<std::string>());
                        return;
                    }

                    onComplete(true, "Success");
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}

void FileEngine::addSyncScans(
    std::filesystem::path const& localPath,
    std::filesystem::path const& remotePath,
    Ids::OperationId remoteScanId,
    Ids::OperationId localScanId,
    bool respectIgnoreFiles,
    bool recursive,
    bool ignoreHidden,
    std::function<void(bool success, std::string const& info)> onComplete
)
{
    Log::info(
        "Requesting to add sync scan: local='{}' remote='{}'",
        localPath.generic_string(),
        remotePath.generic_string()
    );
    lazyOpen(
        [this, localPath, remotePath, remoteScanId, localScanId, respectIgnoreFiles, recursive, ignoreHidden,
            onComplete = std::move(onComplete)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot add sync scan, no channel");
                onComplete(false, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addSyncScan", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add sync scan: {}", val["error"].as<std::string>());
                        onComplete(false, val["error"].as<std::string>());
                        return;
                    }
                    onComplete(true, "Success");
                },
                channelId.value().value(),
                remoteScanId.value(),
                localScanId.value(),
                remotePath.generic_string(),
                localPath.generic_string(),
                respectIgnoreFiles,
                recursive,
                ignoreHidden
            );
        }
    );
}

void FileEngine::addDownload(
    NuiFileExplorer::Item const& remotePath,
    NuiFileExplorer::Item const& localPath,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
    bool allowOverwrite,
    bool insertRefresh,
    bool createMissingDirectories,
    SharedData::OperationMode mode
)
{
    Log::info(
        "Requesting to add download: {} -> {}", remotePath.path.generic_string(), localPath.path.generic_string()
    );
    lazyOpen(
        [this,
            remotePath,
            localPath,
            onOperationCreated = std::move(onOperationCreated),
            allowOverwrite,
            insertRefresh,
            createMissingDirectories,
            mode](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot add download, no channel");
                onOperationCreated(std::nullopt, info);
                return;
            }

            const auto operationId = Ids::generateOperationId();

            Log::info(
                "Adding download (with ID '{}'): {} -> {}",
                operationId.value(),
                remotePath.path.generic_string(),
                localPath.path.generic_string()
            );

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addDownload", impl_->engine->sshSessionId().value()),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add download: {}", val["error"].as<std::string>());
                        onOperationCreated(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    onOperationCreated(operationId, "Success");
                },
                channelId.value().value(),
                operationId.value(),
                (!remotePath.fullPath.empty() ? remotePath.fullPath : remotePath.path).generic_string(),
                (!localPath.fullPath.empty() ? localPath.fullPath : localPath.path).generic_string(),
                allowOverwrite,
                remotePath.size > Constants::bigFileCutOff,
                insertRefresh,
                createMissingDirectories,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::addArchiveDownload(
    std::vector<SharedData::DirectoryEntry> entries,
    std::filesystem::path const& localArchivePath,
    int compressionCodec,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated
)
{
    Log::info(
        "Requesting archive download: {} entries → {} (codec={}, level={})",
        entries.size(),
        localArchivePath.generic_string(),
        compressionCodec,
        compressionLevel
    );

    lazyOpen(
        [this,
            entries = std::move(entries),
            localArchivePath,
            compressionCodec,
            compressionLevel,
            mayOverwrite,
            mode,
            onOperationCreated = std::move(onOperationCreated)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot add archive download, no channel");
                onOperationCreated(std::nullopt, info);
                return;
            }

            const auto operationId = Ids::generateOperationId();

            // DirectoryEntry has to_json but no to_val; the RPC wire layer needs
            // Nui::val conversion, so stringify here and parse on the backend.
            nlohmann::json entriesJson = nlohmann::json::array();
            for (auto const& entry : entries)
                entriesJson.push_back(entry);

            Nui::RpcClient::callWithBackChannel(
                fmt::format(
                    "Session::{}::sftp::addArchiveDownload", impl_->engine->sshSessionId().value()
                ),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error(
                            "(Frontend) Failed to add archive download: {}",
                            val["error"].as<std::string>()
                        );
                        onOperationCreated(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    onOperationCreated(operationId, "Success");
                },
                channelId.value().value(),
                operationId.value(),
                entriesJson.dump(),
                localArchivePath.generic_string(),
                compressionCodec,
                compressionLevel,
                mayOverwrite,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::addArchiveUpload(
    std::vector<std::filesystem::path> localPaths,
    std::filesystem::path const& remoteArchivePath,
    int compressionCodec,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated
)
{
    Log::info(
        "Requesting archive upload: {} paths → {} (codec={}, level={})",
        localPaths.size(),
        remoteArchivePath.generic_string(),
        compressionCodec,
        compressionLevel
    );

    lazyOpen(
        [this,
            localPaths = std::move(localPaths),
            remoteArchivePath,
            compressionCodec,
            compressionLevel,
            mayOverwrite,
            mode,
            onOperationCreated = std::move(onOperationCreated)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot add archive upload, no channel");
                onOperationCreated(std::nullopt, info);
                return;
            }

            const auto operationId = Ids::generateOperationId();

            std::vector<std::string> localPathStrings;
            localPathStrings.reserve(localPaths.size());
            for (auto const& localPath : localPaths)
                localPathStrings.push_back(localPath.generic_string());

            Nui::RpcClient::callWithBackChannel(
                fmt::format(
                    "Session::{}::sftp::addArchiveUpload", impl_->engine->sshSessionId().value()
                ),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error(
                            "(Frontend) Failed to add archive upload: {}",
                            val["error"].as<std::string>()
                        );
                        onOperationCreated(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    onOperationCreated(operationId, "Success");
                },
                channelId.value().value(),
                operationId.value(),
                localPathStrings,
                remoteArchivePath.generic_string(),
                compressionCodec,
                compressionLevel,
                mayOverwrite,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::addBulkDownload(
    std::vector<SharedData::BulkAddEntry> entries,
    std::vector<Ids::OperationId> operationIds,
    bool allowOverwrite,
    bool insertRefresh,
    SharedData::OperationMode mode,
    std::function<void(bool success, std::string const& info)> onBulkCreated
)
{
    Log::info("Requesting bulk download for {} entries", entries.size());

    // operationIds carries one id per entry plus one dedicated aggregate-bulk-card id
    // at the back — see OperationQueue::addBulkDownloadOperation for rationale.
    if (entries.size() + 1 != operationIds.size())
    {
        Log::error("addBulkDownload: entries and operationIds size mismatch");
        onBulkCreated(false, "entries and operationIds size mismatch");
        return;
    }

    lazyOpen(
        [this,
            entries = std::move(entries),
            operationIds = std::move(operationIds),
            allowOverwrite,
            insertRefresh,
            mode,
            onBulkCreated = std::move(onBulkCreated)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot bulk-add download, no channel");
                onBulkCreated(false, info);
                return;
            }

            std::vector<std::string> operationIdStrings;
            operationIdStrings.reserve(operationIds.size());
            for (auto const& id : operationIds)
                operationIdStrings.push_back(id.value());

            SharedData::BulkAddRequest request{
                .entries = entries,
                .allowOverwrite = allowOverwrite,
                .insertRefresh = insertRefresh,
                .mode = mode,
            };

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addBulkDownload", impl_->engine->sshSessionId().value()),
                [onBulkCreated = std::move(onBulkCreated)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed bulk download: {}", val["error"].as<std::string>());
                        onBulkCreated(false, val["error"].as<std::string>());
                        return;
                    }
                    onBulkCreated(true, "Success");
                },
                channelId.value().value(),
                operationIdStrings,
                request
            );
        }
    );
}

void FileEngine::addBulkUpload(
    std::vector<SharedData::BulkAddEntry> entries,
    std::vector<Ids::OperationId> operationIds,
    bool allowOverwrite,
    bool insertRefresh,
    SharedData::OperationMode mode,
    std::function<void(bool success, std::string const& info)> onBulkCreated
)
{
    Log::info("Requesting bulk upload for {} entries", entries.size());

    // operationIds carries one id per entry plus one dedicated aggregate-bulk-card id
    // at the back — see OperationQueue::addBulkUploadOperation for rationale.
    if (entries.size() + 1 != operationIds.size())
    {
        Log::error("addBulkUpload: entries and operationIds size mismatch");
        onBulkCreated(false, "entries and operationIds size mismatch");
        return;
    }

    lazyOpen(
        [this,
            entries = std::move(entries),
            operationIds = std::move(operationIds),
            allowOverwrite,
            insertRefresh,
            mode,
            onBulkCreated = std::move(onBulkCreated)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot bulk-add upload, no channel");
                onBulkCreated(false, info);
                return;
            }

            std::vector<std::string> operationIdStrings;
            operationIdStrings.reserve(operationIds.size());
            for (auto const& id : operationIds)
                operationIdStrings.push_back(id.value());

            SharedData::BulkAddRequest request{
                .entries = entries,
                .allowOverwrite = allowOverwrite,
                .insertRefresh = insertRefresh,
                .mode = mode,
            };

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addBulkUpload", impl_->engine->sshSessionId().value()),
                [onBulkCreated = std::move(onBulkCreated)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed bulk upload: {}", val["error"].as<std::string>());
                        onBulkCreated(false, val["error"].as<std::string>());
                        return;
                    }
                    onBulkCreated(true, "Success");
                },
                channelId.value().value(),
                operationIdStrings,
                request
            );
        }
    );
}

void FileEngine::addBulkDelete(
    std::vector<SharedData::BulkAddEntry> entries,
    Ids::OperationId bulkOperationId,
    bool insertRefresh,
    SharedData::OperationMode mode,
    std::function<void(bool success, std::string const& info)> onBulkCreated
)
{
    Log::info("Requesting bulk delete for {} entries", entries.size());

    lazyOpen(
        [this,
            entries = std::move(entries),
            bulkOperationId,
            insertRefresh,
            mode,
            onBulkCreated = std::move(onBulkCreated)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot bulk-delete, no channel");
                onBulkCreated(false, info);
                return;
            }

            SharedData::BulkAddRequest request{
                .entries = entries,
                .allowOverwrite = false,
                .insertRefresh = insertRefresh,
                .mode = mode,
            };

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addBulkDelete", impl_->engine->sshSessionId().value()),
                [onBulkCreated = std::move(onBulkCreated)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed bulk delete: {}", val["error"].as<std::string>());
                        onBulkCreated(false, val["error"].as<std::string>());
                        return;
                    }
                    onBulkCreated(true, "Success");
                },
                channelId.value().value(),
                bulkOperationId.value(),
                request
            );
        }
    );
}

void FileEngine::addUpload(
    NuiFileExplorer::Item const& remotePath,
    NuiFileExplorer::Item const& localPath,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
    bool allowOverwrite,
    bool insertRefresh,
    bool createMissingDirectories,
    SharedData::OperationMode mode
)
{
    Log::info("Requesting to add upload: {} -> {}", localPath.path.generic_string(), remotePath.path.generic_string());
    lazyOpen(
        [this,
            remotePath,
            localPath,
            onOperationCreated = std::move(onOperationCreated),
            allowOverwrite,
            insertRefresh,
            createMissingDirectories,
            mode](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot add upload, no channel");
                onOperationCreated(std::nullopt, info);
                return;
            }

            const auto operationId = Ids::generateOperationId();

            Log::info(
                "Adding upload (with ID '{}'): {} -> {}",
                operationId.value(),
                localPath.path.generic_string(),
                remotePath.path.generic_string()
            );

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addUpload", impl_->engine->sshSessionId().value()),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add upload: {}", val["error"].as<std::string>());
                        onOperationCreated(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    onOperationCreated(operationId, "Success");
                },
                channelId.value().value(),
                operationId.value(),
                (!localPath.fullPath.empty() ? localPath.fullPath : localPath.path).generic_string(),
                (!remotePath.fullPath.empty() ? remotePath.fullPath : remotePath.path).generic_string(),
                allowOverwrite,
                remotePath.size > Constants::bigFileCutOff,
                insertRefresh,
                createMissingDirectories,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::remove(
    std::vector<NuiFileExplorer::Item> const& files,
    std::vector<NuiFileExplorer::Item> const& directories,
    std::function<void(bool, std::string const& info)> onComplete,
    std::function<void(
        std::vector<std::filesystem::path>, /* regular files & empty */
        std::vector<std::filesystem::path> /* non empties */
    )> onNonEmptyDirectoriesFound
)
{
    Log::info("Requesting to remove {} items", files.size() + directories.size());

    lazyOpen(
        [this,
            onComplete = std::move(onComplete),
            files,
            directories,
            onNonEmptyDirectoriesFound =
                std::move(onNonEmptyDirectoriesFound)](auto const& channelId, std::string const& info) mutable
        {
            if (!channelId)
            {
                Log::error("Cannot add upload, no channel");
                onComplete(false, info);
                return;
            }

            std::vector<std::string> transformedDirectories;
            transformedDirectories.resize(directories.size());
            std::transform(
                directories.begin(),
                directories.end(),
                transformedDirectories.begin(),
                [](auto const& item)
                {
                    return item.path.generic_string();
                }
            );

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::preDeleteChecks", impl_->engine->sshSessionId().value()),
                [this,
                    onComplete = std::move(onComplete),
                    files = std::move(files),
                    directories = std::move(directories),
                    onNonEmptyDirectoriesFound = std::move(onNonEmptyDirectoriesFound)](Nui::val val) mutable
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error(
                            "(Frontend) Failed to perform pre-delete checks: {}", val["error"].as<std::string>()
                        );
                        onComplete(false, val["error"].as<std::string>());
                        return;
                    }
                    if (!val.hasOwnProperty("nonEmptyDirectories"))
                    {
                        Log::error("(Frontend) Failed to perform pre-delete checks: no nonEmptyDirectories");
                        onComplete(false, "no nonEmptyDirectories");
                        return;
                    }
                    std::vector<std::filesystem::path> nonEmpties;
                    Nui::convertFromVal(val["nonEmptyDirectories"], nonEmpties);

                    if (nonEmpties.empty())
                    {
                        std::vector<std::filesystem::path> transformedDirectories;
                        transformedDirectories.resize(directories.size());
                        std::transform(
                            directories.begin(),
                            directories.end(),
                            transformedDirectories.begin(),
                            [](auto const& item)
                            {
                                return item.path;
                            }
                        );
                        performDelete(std::move(files), std::move(transformedDirectories), std::move(onComplete));
                    }
                    else
                    {
                        std::vector<std::filesystem::path> filesAndEmptyDirs;
                        filesAndEmptyDirs.reserve(files.size() + (directories.size() - nonEmpties.size()));
                        for (const auto& file : files)
                            filesAndEmptyDirs.push_back(file.path);
                        for (const auto& dir : directories)
                        {
                            if (std::find(nonEmpties.begin(), nonEmpties.end(), dir.path) == nonEmpties.end())
                                filesAndEmptyDirs.push_back(dir.path);
                        }

                        // Dont actually perform delete here immediately, this is something for the queue!
                        onNonEmptyDirectoriesFound(std::move(filesAndEmptyDirs), std::move(nonEmpties));
                    }
                },
                channelId.value().value(),
                transformedDirectories
            );
        }
    );
}

void FileEngine::performDelete(
    std::vector<NuiFileExplorer::Item> files,
    std::vector<std::filesystem::path> directoriesEmpty,
    std::function<void(bool, std::string const& info)> onComplete
)
{
    lazyOpen(
        [this,
            onComplete = std::move(onComplete),
            files = std::move(files),
            directoriesEmpty = std::move(directoriesEmpty)](auto const& channelId, std::string const& info) mutable
        {
            if (!channelId)
            {
                Log::error("Cannot add upload, no channel");
                onComplete(false, info);
                return;
            }

            std::vector<std::string> transformedPaths;
            transformedPaths.reserve(files.size() + directoriesEmpty.size());
            std::transform(
                files.begin(),
                files.end(),
                std::back_inserter(transformedPaths),
                [](auto const& item)
                {
                    return item.path.generic_string();
                }
            );
            std::transform(
                directoriesEmpty.begin(),
                directoriesEmpty.end(),
                std::back_inserter(transformedPaths),
                [](auto const& path)
                {
                    return path.generic_string();
                }
            );

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::deleteFiles", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);

                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to delete files: {}", val["error"].as<std::string>());
                        onComplete(false, val["error"].as<std::string>());
                        return;
                    }
                    onComplete(true, "Success");
                },
                channelId.value().value(),
                transformedPaths
            );
        }
    );
}

void FileEngine::rename(
    std::filesystem::path const& oldPath,
    std::filesystem::path const& newPath,
    std::function<void(bool, std::string const& info)> onComplete
)
{
    Log::info("Requesting to rename file: {} -> {}", oldPath.generic_string(), newPath.generic_string());

    lazyOpen(
        [this, oldPath, newPath, onComplete = std::move(onComplete)](
            auto const& channelId, std::string const& info
        ) mutable
        {
            if (!channelId)
            {
                Log::error("Cannot rename file, no channel");
                onComplete(false, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::rename", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to rename file: {}", val["error"].as<std::string>());
                        onComplete(false, val["error"].as<std::string>());
                        return;
                    }
                    onComplete(true, "Success");
                },
                channelId.value().value(),
                oldPath.generic_string(),
                newPath.generic_string()
            );
        }
    );
}

void FileEngine::addRename(
    std::filesystem::path const& sourcePath,
    std::filesystem::path const& destinationPath,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
    SharedData::OperationMode mode
)
{
    Log::info(
        "Requesting to add rename operation: {} -> {}", sourcePath.generic_string(), destinationPath.generic_string()
    );
    lazyOpen(
        [this, sourcePath, destinationPath, onOperationCreated = std::move(onOperationCreated), mode](
            auto const& channelId, std::string const& info
        ) mutable
        {
            if (!channelId)
            {
                Log::error("Cannot add rename, no channel");
                onOperationCreated(std::nullopt, info);
                return;
            }

            const auto operationId = Ids::generateOperationId();

            Log::info(
                "Adding rename (with ID '{}'): {} -> {}",
                operationId.value(),
                sourcePath.generic_string(),
                destinationPath.generic_string()
            );

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addRename", impl_->engine->sshSessionId().value()),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add rename: {}", val["error"].as<std::string>());
                        onOperationCreated(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    onOperationCreated(operationId, "Success");
                },
                channelId.value().value(),
                operationId.value(),
                sourcePath.generic_string(),
                destinationPath.generic_string(),
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::removeOnQueueUnchecked(
    std::vector<std::filesystem::path> const& paths,
    bool recursive,
    std::function<void(std::optional<std::vector<Ids::OperationId>> const&, std::string const& info)> onComplete,
    SharedData::OperationMode mode
)
{
    lazyOpen(
        [this, paths, recursive, onComplete = std::move(onComplete), mode](
            auto const& channelId, std::string const& info
        ) mutable
        {
            if (!channelId)
            {
                Log::error("Cannot add upload, no channel");
                onComplete(std::nullopt, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::queuedRemove", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val) mutable
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to delete files: {}", val["error"].as<std::string>());
                        onComplete(std::nullopt, val["error"].as<std::string>());
                        return;
                    }

                    std::vector<Ids::OperationId> operationIds;
                    const auto ids = val["operationIds"];
                    for (const auto& idVal : ids)
                        operationIds.push_back(Ids::makeOperationId(idVal.as<std::string>()));

                    onComplete(operationIds, "Success");
                },
                channelId.value().value(),
                paths,
                recursive,
                true,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::existsBatchRemote(
    std::vector<std::filesystem::path> const& paths,
    std::function<void(std::vector<bool> exists, std::string const& info)> onComplete
)
{
    Log::info("Requesting existsBatch for {} remote paths", paths.size());

    std::vector<std::string> pathStrings;
    pathStrings.reserve(paths.size());
    for (auto const& path : paths)
        pathStrings.push_back(path.generic_string());

    lazyOpen(
        [this,
            pathStrings = std::move(pathStrings),
            onComplete = std::move(onComplete)](auto const& channelId, std::string const& info) mutable
        {
            if (!channelId)
            {
                Log::error("Cannot run existsBatchRemote, no channel");
                onComplete({}, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::existsBatch", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) sftp::existsBatch failed: {}", val["error"].as<std::string>());
                        onComplete({}, val["error"].as<std::string>());
                        return;
                    }
                    if (!val.hasOwnProperty("success") || !val["success"].as<bool>() ||
                        !val.hasOwnProperty("exists"))
                    {
                        onComplete({}, "Malformed sftp::existsBatch response");
                        return;
                    }
                    auto arr = val["exists"];
                    const auto length = arr["length"].as<long long>();
                    std::vector<bool> results;
                    results.reserve(static_cast<std::size_t>(length));
                    for (long long idx = 0; idx < length; ++idx)
                        results.push_back(arr[static_cast<int>(idx)].as<bool>());
                    onComplete(std::move(results), "Success");
                },
                channelId.value().value(),
                pathStrings
            );
        }
    );
}

void FileEngine::stat(
    std::filesystem::path const& path,
    std::function<
        void(std::optional<std::pair<bool /*exists*/, SharedData::DirectoryEntry>> const&, std::string const& info)>
        onComplete
)
{
    Log::info("Requesting info of file: {}", path.generic_string());

    lazyOpen(
        [this, path, onComplete = std::move(onComplete)](auto const& channelId, std::string const& info) mutable
        {
            if (!channelId)
            {
                Log::error("Cannot stat file, no channel");
                onComplete(std::nullopt, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::stat", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    Nui::WebApi::Console::log("stat val", val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to rename file: {}", val["error"].as<std::string>());
                        onComplete(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    if (!val.hasOwnProperty("stat") || val["stat"].isNull() || val["stat"].isUndefined())
                    {
                        onComplete(
                            std::pair<bool /*exists*/, SharedData::DirectoryEntry>{false, {}}, "File does not exist"
                        );
                        return;
                    }
                    onComplete(
                        std::pair<bool /*exists*/, SharedData::DirectoryEntry>{
                            true, nlohmann::json::parse(Nui::JSON::stringify(val))["stat"]
                        },
                        "Success"
                    );
                },
                channelId.value().value(),
                path.generic_string()
            );
        }
    );
}
