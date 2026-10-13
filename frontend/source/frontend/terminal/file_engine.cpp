#include <frontend/terminal/file_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <log/log.hpp>
#include <constants/sftp.hpp>

#include <nui/rpc.hpp>
#include <nui/frontend/api/json.hpp>

#include <utility>

struct FileEngine::Implementation
{
    bool wasDisposed = false;
    SshTerminalEngine* engine;
    std::optional<Ids::ChannelId> sftpChannelId{std::nullopt};
    std::vector<std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)>> pendingOpens{};

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

    impl_->pendingOpens.push_back(onOpen);
    if (impl_->pendingOpens.size() > 1)
        return;

    Log::info("Creating sftp channel");
    impl_->engine->createSftpChannel(
        [this](auto const& id, std::string const& info)
        {
            impl_->sftpChannelId = id;
            for (auto const& pendingOpen : std::exchange(impl_->pendingOpens, {}))
                pendingOpen(id, info);
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

void FileEngine::homeDirectory(
    std::function<void(std::optional<std::filesystem::path> const&, std::string const& info)> onComplete
)
{
    lazyOpen(
        [this, onComplete = std::move(onComplete)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot get home directory, no sftp channel");
                onComplete(std::nullopt, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::homeDirectory", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    if (val.hasOwnProperty("error") || !val.hasOwnProperty("path"))
                    {
                        const auto error =
                            val.hasOwnProperty("error") ? val["error"].as<std::string>() : std::string{"No path"};
                        Log::error("(Frontend) Failed to get home directory: {}", error);
                        onComplete(std::nullopt, error);
                        return;
                    }

                    onComplete(std::filesystem::path{val["path"].as<std::string>()}, "Success");
                },
                channelId.value().value()
            );
        }
    );
}

void FileEngine::userDirectories(
    std::filesystem::path const& home,
    std::function<
        void(std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> const&, std::string const& info)>
        onComplete
)
{
    lazyOpen(
        [this, home, onComplete = std::move(onComplete)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot read user directories, no sftp channel");
                onComplete(std::nullopt, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::userDirectories", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    if (val.hasOwnProperty("error") || !val.hasOwnProperty("directories"))
                    {
                        const auto error = val.hasOwnProperty("error") ? val["error"].as<std::string>()
                                                                       : std::string{"No directories"};
                        Log::warn("(Frontend) Failed to read user directories: {}", error);
                        onComplete(std::nullopt, error);
                        return;
                    }

                    std::vector<std::pair<std::string, std::filesystem::path>> directories;
                    const auto list = val["directories"];
                    const auto length = list["length"].as<int>();
                    directories.reserve(length);
                    for (int index = 0; index < length; ++index)
                    {
                        directories.emplace_back(
                            list[index]["name"].as<std::string>(),
                            std::filesystem::path{list[index]["path"].as<std::string>()}
                        );
                    }
                    onComplete(std::move(directories), "Success");
                },
                channelId.value().value(),
                home.generic_string()
            );
        }
    );
}

void FileEngine::createDirectory(
    Paths::RemotePath const& path,
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

            Log::info("Creating directory: {}", path.value());
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
                path.value()
            );
        }
    );
}

void FileEngine::createFile(
    Paths::RemotePath const& path,
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

            Log::info("Creating file: {}", path.value());
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
                path.value()
            );
        }
    );
}

void FileEngine::openSyncSession(
    std::filesystem::path const& localPath,
    std::filesystem::path const& remotePath,
    Ids::SyncSessionId syncSessionId,
    Ids::OperationId remoteScanId,
    Ids::OperationId localScanId,
    bool respectIgnoreFiles,
    bool recursive,
    bool ignoreHidden,
    std::function<void(bool success, std::string const& info)> onComplete
)
{
    Log::info(
        "Requesting openSyncSession: local='{}' remote='{}'", localPath.generic_string(), remotePath.generic_string()
    );
    lazyOpen(
        [this,
            localPath,
            remotePath,
            syncSessionId,
            remoteScanId,
            localScanId,
            respectIgnoreFiles,
            recursive,
            ignoreHidden,
            onComplete = std::move(onComplete)](auto const& channelId, std::string const& info)
        {
            if (!channelId)
            {
                Log::error("Cannot open sync session, no channel");
                onComplete(false, info);
                return;
            }

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::openSyncSession", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete)](Nui::val val)
                {
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to open sync session: {}", val["error"].as<std::string>());
                        onComplete(false, val["error"].as<std::string>());
                        return;
                    }
                    onComplete(true, "Success");
                },
                channelId.value().value(),
                syncSessionId.value(),
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

void FileEngine::recomputeSyncDiff(
    Ids::SyncSessionId syncSessionId,
    SharedData::Sync::DiffOptions options,
    SharedData::Sync::LoadedParents loadedParents,
    std::function<void(SharedData::Sync::DiffSnapshot)> onSnapshot
)
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::sftp::recomputeSyncDiff", impl_->engine->sshSessionId().value()),
        [onSnapshot = std::move(onSnapshot)](Nui::val val)
        {
            const auto failed = [&onSnapshot]()
            {
                SharedData::Sync::DiffSnapshot empty{};
                empty.summary.cancelled = true;
                onSnapshot(std::move(empty));
            };
            if (val.hasOwnProperty("error"))
            {
                Log::error("recomputeSyncDiff failed: {}", val["error"].as<std::string>());
                return failed();
            }
            try
            {
                const auto json = nlohmann::json::parse(Nui::JSON::stringify(val));
                onSnapshot(json.get<SharedData::Sync::DiffSnapshot>());
            }
            catch (std::exception const& exc)
            {
                Log::error("Failed to parse DiffSnapshot: {}", exc.what());
                failed();
            }
        },
        syncSessionId.value(),
        options,
        loadedParents
    );
}

void FileEngine::loadSyncDiffChildren(
    Ids::SyncSessionId syncSessionId,
    SharedData::Sync::DiffSection section,
    std::string const& parentRelKey,
    std::uint64_t generation,
    std::function<void(std::vector<SharedData::Sync::DiffTreeNode>)> onResolved,
    std::function<void(std::string const&)> onRejected
)
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::sftp::loadSyncDiffChildren", impl_->engine->sshSessionId().value()),
        [onResolved = std::move(onResolved), onRejected = std::move(onRejected)](Nui::val val)
        {
            if (val.hasOwnProperty("error"))
            {
                onRejected(val["error"].as<std::string>());
                return;
            }
            try
            {
                const auto json = nlohmann::json::parse(Nui::JSON::stringify(val));
                std::vector<SharedData::Sync::DiffTreeNode> nodes;
                nodes.reserve(json.size());
                for (auto const& elem : json)
                    nodes.push_back(elem.get<SharedData::Sync::DiffTreeNode>());
                onResolved(std::move(nodes));
            }
            catch (std::exception const& exc)
            {
                onRejected(exc.what());
            }
        },
        syncSessionId.value(),
        section,
        parentRelKey,
        std::to_string(generation)
    );
}

void FileEngine::executeSyncSelection(
    SharedData::Sync::SyncExecutionRequest request,
    std::function<void(SharedData::Sync::SyncExecutionResult)> onResolved,
    std::function<void(std::string const&)> onRejected
)
{
    lazyOpen(
        [this, request = std::move(request), onResolved = std::move(onResolved), onRejected = std::move(onRejected)](
            auto const& channelId, std::string const& info
        ) mutable
        {
            if (!channelId)
            {
                onRejected(info);
                return;
            }
            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::executeSyncSelection", impl_->engine->sshSessionId().value()),
                [onResolved = std::move(onResolved), onRejected = std::move(onRejected)](Nui::val val)
                {
                    if (val.hasOwnProperty("error"))
                    {
                        onRejected(val["error"].as<std::string>());
                        return;
                    }
                    try
                    {
                        onResolved(
                            nlohmann::json::parse(Nui::JSON::stringify(val)).get<SharedData::Sync::SyncExecutionResult>()
                        );
                    }
                    catch (std::exception const& exception)
                    {
                        onRejected(exception.what());
                    }
                },
                channelId.value().value(),
                request
            );
        }
    );
}

void FileEngine::cancelSyncDiff(Ids::SyncSessionId syncSessionId)
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::sftp::cancelSyncDiff", impl_->engine->sshSessionId().value()),
        [](Nui::val /*val*/) {},
        syncSessionId.value()
    );
}

void FileEngine::closeSyncSession(Ids::SyncSessionId syncSessionId)
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::sftp::closeSyncSession", impl_->engine->sshSessionId().value()),
        [](Nui::val /*val*/) {},
        syncSessionId.value()
    );
}

void FileEngine::addDownload(
    Paths::RemotePath const& source,
    Paths::LocalPath const& destination,
    std::uint64_t sizeBytes,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
    bool allowOverwrite,
    bool insertRefresh,
    bool createMissingDirectories,
    SharedData::OperationMode mode
)
{
    Log::info("Requesting to add download: {} -> {}", source.value(), destination.toUtf8());
    lazyOpen(
        [this,
            source,
            destination,
            sizeBytes,
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
                "Adding download (with ID '{}'): {} -> {}", operationId.value(), source.value(), destination.toUtf8()
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
                source.value(),
                destination.toUtf8(),
                allowOverwrite,
                sizeBytes > Constants::bigFileCutOff,
                insertRefresh,
                createMissingDirectories,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::addArchiveDownload(
    std::vector<SharedData::ArchiveDownloadRoot> roots,
    Paths::LocalPath const& localArchivePath,
    int compressionCodec,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated
)
{
    Log::info(
        "Requesting archive download: {} roots → {} (codec={}, level={})",
        roots.size(),
        localArchivePath.toUtf8(),
        compressionCodec,
        compressionLevel
    );

    lazyOpen(
        [this,
            roots = std::move(roots),
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

            // The roots have to_json but no to_val; the RPC wire layer needs
            // Nui::val conversion, so stringify here and parse on the backend.
            const nlohmann::json rootsJson = roots;

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addArchiveDownload", impl_->engine->sshSessionId().value()),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add archive download: {}", val["error"].as<std::string>());
                        onOperationCreated(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    onOperationCreated(operationId, "Success");
                },
                channelId.value().value(),
                operationId.value(),
                rootsJson.dump(),
                localArchivePath.toUtf8(),
                compressionCodec,
                compressionLevel,
                mayOverwrite,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::addArchiveUpload(
    std::vector<Paths::LocalPath> localPaths,
    Paths::RemotePath const& remoteArchivePath,
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
        remoteArchivePath.value(),
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
                localPathStrings.push_back(localPath.toUtf8());

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::addArchiveUpload", impl_->engine->sshSessionId().value()),
                [onOperationCreated = std::move(onOperationCreated), operationId](Nui::val val)
                {
                    Nui::WebApi::Console::log(val);
                    if (val.hasOwnProperty("error"))
                    {
                        Log::error("(Frontend) Failed to add archive upload: {}", val["error"].as<std::string>());
                        onOperationCreated(std::nullopt, val["error"].as<std::string>());
                        return;
                    }
                    onOperationCreated(operationId, "Success");
                },
                channelId.value().value(),
                operationId.value(),
                localPathStrings,
                remoteArchivePath.value(),
                compressionCodec,
                compressionLevel,
                mayOverwrite,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::addBulkDownload(
    std::vector<SharedData::BulkDownloadEntry> entries,
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

            SharedData::BulkDownloadRequest request{
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
    std::vector<SharedData::BulkUploadEntry> entries,
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

            SharedData::BulkUploadRequest request{
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
    std::vector<SharedData::BulkDeleteEntry> entries,
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

            SharedData::BulkDeleteRequest request{
                .entries = entries,
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
    Paths::LocalPath const& source,
    Paths::RemotePath const& destination,
    std::uint64_t sizeBytes,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
    bool allowOverwrite,
    bool insertRefresh,
    bool createMissingDirectories,
    SharedData::OperationMode mode
)
{
    Log::info("Requesting to add upload: {} -> {}", source.toUtf8(), destination.value());
    lazyOpen(
        [this,
            source,
            destination,
            sizeBytes,
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
                "Adding upload (with ID '{}'): {} -> {}", operationId.value(), source.toUtf8(), destination.value()
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
                source.toUtf8(),
                destination.value(),
                allowOverwrite,
                sizeBytes > Constants::bigFileCutOff,
                insertRefresh,
                createMissingDirectories,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::remove(
    std::vector<Paths::RemotePath> files,
    std::vector<Paths::RemotePath> directories,
    std::function<void(bool, std::string const& info)> onComplete,
    std::function<void(std::vector<Paths::RemotePath>, std::vector<Paths::RemotePath>)> onNonEmptyDirectoriesFound
)
{
    Log::info("Requesting to remove {} items", files.size() + directories.size());

    lazyOpen(
        [this,
            onComplete = std::move(onComplete),
            files = std::move(files),
            directories = std::move(directories),
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
            transformedDirectories.reserve(directories.size());
            for (auto const& directory : directories)
                transformedDirectories.push_back(directory.value());

            Nui::RpcClient::callWithBackChannel(
                fmt::format("Session::{}::sftp::preDeleteChecks", impl_->engine->sshSessionId().value()),
                [onComplete = std::move(onComplete),
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
                    std::vector<std::string> nonEmptyNames;
                    Nui::convertFromVal(val["nonEmptyDirectories"], nonEmptyNames);

                    // Only directories that were asked about count, the answer never adds a delete target.
                    std::vector<Paths::RemotePath> nonEmpties;
                    std::vector<Paths::RemotePath> filesAndEmptyDirs = std::move(files);
                    for (auto& directory : directories)
                    {
                        if (std::ranges::find(nonEmptyNames, directory.value()) != nonEmptyNames.end())
                            nonEmpties.push_back(std::move(directory));
                        else
                            filesAndEmptyDirs.push_back(std::move(directory));
                    }

                    // Always route through the queue (bulk delete), never a synchronous
                    // deleteFiles RPC: a large selection blows the single futureTimeout
                    // even though the server keeps deleting, surfacing a spurious
                    // "Failed to delete files: timeout". nonEmpties may be empty, in which
                    // case the caller enqueues the file/empty-dir batch immediately.
                    onNonEmptyDirectoriesFound(std::move(filesAndEmptyDirs), std::move(nonEmpties));
                },
                channelId.value().value(),
                transformedDirectories
            );
        }
    );
}

void FileEngine::rename(
    Paths::RemotePath const& oldPath,
    Paths::RemotePath const& newPath,
    bool overwrite,
    std::function<void(bool, std::string const& info)> onComplete
)
{
    Log::info("Requesting to rename file: {} -> {}", oldPath.value(), newPath.value());

    lazyOpen(
        [this, oldPath, newPath, overwrite, onComplete = std::move(onComplete)](
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
                oldPath.value(),
                newPath.value(),
                overwrite
            );
        }
    );
}

void FileEngine::addRename(
    Paths::RemotePath const& sourcePath,
    Paths::RemotePath const& destinationPath,
    std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
    SharedData::OperationMode mode
)
{
    Log::info("Requesting to add rename operation: {} -> {}", sourcePath.value(), destinationPath.value());
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
                sourcePath.value(),
                destinationPath.value()
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
                sourcePath.value(),
                destinationPath.value(),
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::removeOnQueueUnchecked(
    std::vector<Paths::RemotePath> const& paths,
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

            std::vector<std::string> pathStrings;
            pathStrings.reserve(paths.size());
            for (auto const& path : paths)
                pathStrings.push_back(path.value());

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
                pathStrings,
                recursive,
                true,
                static_cast<int>(mode)
            );
        }
    );
}

void FileEngine::existsBatchRemote(
    std::vector<Paths::RemotePath> const& paths,
    std::function<void(std::vector<bool> exists, std::string const& info)> onComplete
)
{
    Log::info("Requesting existsBatch for {} remote paths", paths.size());

    std::vector<std::string> pathStrings;
    pathStrings.reserve(paths.size());
    for (auto const& path : paths)
        pathStrings.push_back(path.value());

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
                    if (!val.hasOwnProperty("success") || !val["success"].as<bool>() || !val.hasOwnProperty("exists"))
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
