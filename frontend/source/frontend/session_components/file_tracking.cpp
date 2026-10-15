#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/file_tracking_state.hpp>
#include <frontend/session_components/operation_queue.hpp>

#include <paths/paths.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/language.hpp>
#include <utility/lifetime_token.hpp>
#include <log/log.hpp>

#include <script-nui-components/resizeable_table.hpp>
#include <script-nui-components/button.hpp>
#include <script-nui-components/message_strip.hpp>
#include <script-nui-components/switch.hpp>
#include <script-nui-components/style_variant.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/event_system/event_context.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/rpc.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::string_literals;

struct FileTrackingPanel::Implementation
{
    /**
     * @brief One downloaded remote file or directory whose local copy is watched.
     */
    struct TrackedEntry
    {
        Ids::InstanceId instanceId;
        Paths::LocalPath instanceDirectory;
        Paths::RemotePath remotePath;
        Paths::LocalPath localPath;
        bool autoReupload{true};
        bool moveRemoteOnLocalMove{false};
        bool deleteRemote{false};
        /** @brief Ask before replacing a server copy that changed since it was opened or last uploaded. */
        bool checkRemoteChanges{true};
        /** @brief Whether uploads are on their way, shared with the status cell of the entry's row. */
        std::shared_ptr<Nui::Observed<bool>> uploading{std::make_shared<Nui::Observed<bool>>(false)};
        FileTracking::PendingUploads pendingUploads{};
        FileTracking::KnownServerContent serverContent{};
    };

    /**
     * @brief A changed path of a tracked entry on its way to the server.
     */
    struct Upload
    {
        Ids::InstanceId instanceId;
        FileTracking::LocalChange change;
        Paths::RemotePath destination;
        /** @brief Compare with the server first, when the entry asks for that. */
        bool checkServer{true};
    };

    /**
     * @brief How the upload of a change ended.
     */
    enum class UploadOutcome
    {
        Uploaded,
        NotUploaded,
    };

    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    ConfirmDialog* confirmDialog;

    OperationQueue* operationQueue{nullptr};
    Ids::SessionId sessionId{};
    std::string downloadTempSuffix{Persistence::defaultTempFileSuffix};

    std::map<Ids::InstanceId, TrackedEntry> entries{};
    FileTracking::RowOrder<Ids::InstanceId> rows{};
    Nui::Observed<std::size_t> entryCount{std::size_t{0}};
    std::map<Ids::InstanceId, Nui::RpcClient::AutoUnregister> fileChangeListeners{};
    /** @brief Server-changed questions waiting for the one on screen, so no question replaces another. */
    std::deque<Upload> serverChangedQuestions{};
    bool serverChangedQuestionOpen{false};

    Nui::Observed<int> orphanedCount{-1};
    Nui::Observed<bool> cleanupRunning{false};

    std::shared_ptr<ScriptNuiComponents::ResizableTable> table{std::make_shared<ScriptNuiComponents::ResizableTable>(
        ScriptNuiComponents::ResizableTable::HeaderRow{
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnName")},
                .initialWidth = 0,
                .resizeable = true,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnRemotePath")},
                .initialWidth = 0,
                .resizeable = true,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnAutoReupload")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnMoveRemote")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnDeleteRemote")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnCheckRemote")},
                .initialWidth = 120,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{language->get("fileTracking", "columnStatus")},
                .initialWidth = 100,
                .resizeable = false,
            },
            ScriptNuiComponents::ResizableTable::HeaderTableCell{
                .content = std::string{},
                .initialWidth = 60,
                .resizeable = false,
            },
        },
        std::nullopt
    )};

    Utility::LifetimeToken lifetime{};

    Implementation(Persistence::StateHolder* stateHolder, FrontendEvents* events, ConfirmDialog* confirmDialog)
        : stateHolder{stateHolder}
        , events{events}
        , confirmDialog{confirmDialog}
    {}

    /**
     * @brief Shows what RPC replies and change events changed, they arrive outside of DOM event handlers.
     */
    static void syncView()
    {
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    TrackedEntry* findEntry(Ids::InstanceId const& instanceId)
    {
        const auto found = entries.find(instanceId);
        return found == entries.end() ? nullptr : &found->second;
    }

    void refreshOrphanedCount()
    {
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::countOrphaned",
            lifetime.guarded(
                [this](Nui::val response)
                {
                    if (!response.hasOwnProperty("success") || !response["success"].as<bool>())
                    {
                        Log::error("FileTracking: countOrphaned RPC returned failure or no 'success' field");
                        return;
                    }
                    orphanedCount = response["count"].as<int>();
                    syncView();
                }
            )
        );
    }

    void triggerCleanup()
    {
        cleanupRunning = true;
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::forceCleanup",
            lifetime.guarded(
                [this](Nui::val response)
                {
                    cleanupRunning = false;
                    const bool succeeded = response.hasOwnProperty("success") && response["success"].as<bool>();
                    if (!succeeded)
                        Log::error("FileTracking: forceCleanup failed");
                    else if (response.hasOwnProperty("removed"))
                        Log::info(
                            "FileTracking: forceCleanup removed {} orphaned instance(s)", response["removed"].as<int>()
                        );
                    if (succeeded)
                        refreshOrphanedCount();
                    syncView();
                }
            )
        );
    }

    Nui::ElementRenderer renderHeader()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return header{
            class_ = "file-tracking-header",
        }(
            div{class_ = "file-tracking-header-stats"}(
                span{class_ = "file-tracking-stat"}(
                    span{class_ = "file-tracking-stat-label"}(
                        Nui::Elements::text{std::string{language->get("fileTracking", "headerActive")}}()
                    ),
                    span{class_ = "file-tracking-stat-value"}(
                        Nui::observe(entryCount).generate([this]() -> Nui::ElementRenderer
                        {
                            return Nui::Elements::text{fmt::format("{}", entryCount.value())}();
                        })
                    )
                ),
                span{class_ = "file-tracking-stat"}(
                    span{class_ = "file-tracking-stat-label"}(
                        Nui::Elements::text{std::string{language->get("fileTracking", "headerOrphaned")}}()
                    ),
                    span{class_ = "file-tracking-stat-value file-tracking-stat-orphaned"}(
                        Nui::observe(orphanedCount).generate([this]() -> Nui::ElementRenderer
                        {
                            if (orphanedCount.value() < 0)
                                return Nui::Elements::text{"-"}();
                            return Nui::Elements::text{fmt::format("{}", orphanedCount.value())}();
                        })
                    )
                )
            ),
            div{class_ = "file-tracking-header-actions"}(
                ScriptNuiComponents::button({
                    .text = Nui::observe(cleanupRunning).generate([](bool running) -> std::string
                    {
                        return std::string{language->get("fileTracking", running ? "headerCleanupRunning" : "headerCleanupButton")};
                    }),
                    .attributes =
                        {
                            Nui::Attributes::onClick = lifetime.guarded(
                                [this](Nui::val)
                                {
                                    if (!cleanupRunning.value())
                                        triggerCleanup();
                                }
                            ),
                            disabled = Nui::observe(cleanupRunning).generate([](bool running) { return running; }),
                        },
                    .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                })
            )
        );
        // clang-format on
    }

    /**
     * @brief A cell with a switch for the @p option of the entry @p instanceId.
     */
    ScriptNuiComponents::ResizableTable::TableCell
    switchCell(Ids::InstanceId const& instanceId, bool isChecked, bool TrackedEntry::* option)
    {
        using namespace ScriptNuiComponents;
        return ResizableTable::TableCell{
            [this, instanceId, isChecked, option](std::unique_ptr<ResizableTable::ISelfController>)
                -> Nui::ElementRenderer
            {
                return ScriptNuiComponents::switch_({
                    .isChecked = isChecked,
                    .onChange = lifetime.guarded(
                        [this, instanceId, option](bool checked, Nui::WebApi::MouseEvent const&)
                        {
                            if (auto* entry = findEntry(instanceId))
                                entry->*option = checked;
                        }
                    ),
                });
            },
        };
    }

    ScriptNuiComponents::ResizableTable::TableRow buildRowForEntry(TrackedEntry const& entry)
    {
        using namespace ScriptNuiComponents;
        const auto& instanceId = entry.instanceId;

        return ResizableTable::TableRow{
            entry.localPath.filename() ? std::string{entry.localPath.filename()->view()} : entry.localPath.toUtf8(),
            entry.remotePath.value(),
            switchCell(instanceId, entry.autoReupload, &TrackedEntry::autoReupload),
            switchCell(instanceId, entry.moveRemoteOnLocalMove, &TrackedEntry::moveRemoteOnLocalMove),
            switchCell(instanceId, entry.deleteRemote, &TrackedEntry::deleteRemote),
            switchCell(instanceId, entry.checkRemoteChanges, &TrackedEntry::checkRemoteChanges),
            ResizableTable::TableCell{
                [uploading = entry.uploading](std::unique_ptr<ResizableTable::ISelfController>) -> Nui::ElementRenderer
                {
                    return Nui::Elements::div{
                        Nui::Attributes::class_ = "file-tracking-status",
                        Nui::Attributes::style = "display:contents",
                    }(Nui::observe(*uploading)
                            .generate(
                                [](bool isUploading) -> Nui::ElementRenderer
                                {
                                    return Nui::Elements::text{std::string{language->get(
                                        "fileTracking", isUploading ? "statusUploading" : "statusWatching"
                                    )}}();
                                }
                            ));
                },
            },
            ResizableTable::TableCell{
                [this, instanceId](std::unique_ptr<ResizableTable::ISelfController>) -> Nui::ElementRenderer
                {
                    return ScriptNuiComponents::button({
                        .text = std::string{language->get("fileTracking", "stopButton")},
                        .attributes =
                            {
                                Nui::Attributes::onClick = lifetime.guarded(
                                    [this, instanceId](Nui::val)
                                    {
                                        stopWatching(instanceId);
                                    }
                                ),
                            },
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    });
                },
            },
        };
    }

    void stopWatching(Ids::InstanceId const& instanceId)
    {
        Log::info("FileTracking: stopping watch for instance {}", instanceId.value());
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::destroyInstance",
            lifetime.guarded(
                [this, instanceId](Nui::val response)
                {
                    if (!response.hasOwnProperty("success") || !response["success"].as<bool>())
                    {
                        Log::error("FileTracking: destroyInstance failed for {}", instanceId.value());
                        return;
                    }
                    forgetEntry(instanceId);
                    syncView();
                }
            ),
            instanceId.value()
        );
    }

    void forgetEntry(Ids::InstanceId const& instanceId)
    {
        fileChangeListeners.erase(instanceId);
        entries.erase(instanceId);
        if (const auto row = rows.remove(instanceId))
            table->remove(*row);
        std::erase_if(
            serverChangedQuestions,
            [&instanceId](Upload const& upload)
            {
                return upload.instanceId == instanceId;
            }
        );
        entryCount = entries.size();
    }

    std::expected<Paths::RemotePath, Paths::PathError>
    remotePathOf(TrackedEntry const& entry, Paths::LocalPath const& local) const
    {
        return FileTracking::remotePathOfChange(local, entry.instanceDirectory, entry.remotePath);
    }

    /**
     * @brief Records the states of a FileTracking::fileStates reply, only those below @p below when given.
     */
    static void rememberFileStates(TrackedEntry& entry, Nui::val const& files, std::optional<Paths::LocalPath> const& below)
    {
        const auto count = files["length"].as<int>();
        for (int index = 0; index < count; ++index)
        {
            const auto file = files[index];
            const auto path = Paths::LocalPath::fromUtf8(file["path"].as<std::string>());
            if (!path || (below && !path->isWithin(*below)))
                continue;
            if (file.hasOwnProperty("directory"))
            {
                entry.serverContent.rememberDirectory(*path);
                continue;
            }
            entry.serverContent.rememberFile(
                *path,
                FileTracking::ServerState{
                    .mtime = static_cast<std::uint64_t>(file["mtime"].as<double>()),
                    .size = static_cast<std::uint64_t>(file["size"].as<double>()),
                }
            );
        }
    }

    /**
     * @brief Asks the backend for the files of @p instanceId: they carry the server's modification time after a
     *        download or an upload. @p onDone runs afterwards, also when the reply failed.
     */
    void requestFileStates(
        Ids::InstanceId const& instanceId,
        std::optional<Paths::LocalPath> below,
        std::function<void()> onDone
    )
    {
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::fileStates",
            lifetime.guarded(
                [this, instanceId, below = std::move(below), onDone = std::move(onDone)](Nui::val reply)
                {
                    if (auto* entry = findEntry(instanceId); entry && reply.hasOwnProperty("files"))
                        rememberFileStates(*entry, reply["files"], below);
                    else if (entry)
                        Log::warn("FileTracking: no file states for instance {}, re-uploads will ask.", instanceId.value());
                    onDone();
                }
            ),
            instanceId.value()
        );
    }

    void onFileChanged(Ids::InstanceId const& instanceId, Nui::val const& payload)
    {
        auto* entry = findEntry(instanceId);
        if (!entry)
            return;

        const auto action = payload["action"].as<std::string>();
        const auto directory = payload["directory"].as<std::string>();
        const auto filename = payload["filename"].as<std::string>();
        Log::debug("FileTracking: file changed in instance {}: {} {}", instanceId.value(), action, filename);

        const auto localPathOfChange = [&directory](std::string const& name)
        {
            return Paths::LocalPath::fromUtf8(fmt::format("{}/{}", directory, name));
        };
        // Only the tracked file or directory maps to the remote side, anything else in the instance
        // directory (Finder's .DS_Store for one) would land next to it on the server.
        const auto isTracked = [entry](std::expected<Paths::LocalPath, Paths::PathError> const& changed)
        {
            return changed && changed->isWithin(entry->localPath);
        };
        const auto changedPath = localPathOfChange(filename);
        if (!isTracked(changedPath))
        {
            Log::debug("FileTracking: ignoring change outside the tracked item: {}", filename);
            return;
        }
        const FileTracking::LocalChange change{
            .path = *changedPath,
            .isDirectory = payload.hasOwnProperty("isDirectory") && payload["isDirectory"].as<bool>(),
        };

        if (action == "Deleted")
            return onDeleted(*entry, change.path);

        // A move from outside the tracked item into it (an atomic save renaming a temporary over it) is a change.
        const auto oldPath = localPathOfChange(payload["oldFilename"].as<std::string>());
        if (action == "Moved" && isTracked(oldPath))
            return onMoved(*entry, *oldPath, change);

        onContentChanged(*entry, change);
    }

    /**
     * @param checkServer Compare with the server first, when the entry asks for that. Changes below a directory
     *        that was just uploaded skip it, the states recorded after that upload may already be newer.
     */
    void onContentChanged(TrackedEntry& entry, FileTracking::LocalChange const& change, bool checkServer = true)
    {
        if (!entry.autoReupload || !operationQueue)
            return;
        const auto destination = remotePathOf(entry, change.path);
        if (!destination)
        {
            Log::warn(
                "FileTracking: not uploading the change in instance {}: {}",
                entry.instanceId.value(),
                destination.error().toString()
            );
            return;
        }
        queueUpload(Upload{
            .instanceId = entry.instanceId,
            .change = change,
            .destination = *destination,
            .checkServer = checkServer,
        });
    }

    void onDeleted(TrackedEntry& entry, Paths::LocalPath const& path)
    {
        if (!entry.deleteRemote || !operationQueue)
            return;
        if (entry.pendingUploads.removeRemoteWhenDone(path))
            return;
        if (!entry.serverContent.has(path))
        {
            Log::debug("FileTracking: '{}' is not on the server, nothing to delete there.", path.toUtf8());
            return;
        }
        // Never recursive: a directory deleted entry by entry reports its content first, so only one moved away as a
        // whole (to the trash) still has known content, and its server copy keeps it.
        if (entry.serverContent.hasBelow(path))
        {
            Log::info(
                "FileTracking: not deleting the server's copy of directory '{}', it was removed with its content.",
                path.toUtf8()
            );
            return;
        }
        const auto remotePath = remotePathOf(entry, path);
        if (!remotePath)
        {
            Log::error(
                "FileTracking: not mirroring the deletion of '{}': {}", path.toUtf8(), remotePath.error().toString()
            );
            return;
        }
        removeRemote(entry, path, *remotePath);
    }

    void onMoved(TrackedEntry& entry, Paths::LocalPath const& oldPath, FileTracking::LocalChange const& change)
    {
        const auto oldRemotePath = remotePathOf(entry, oldPath);
        const auto newRemotePath = remotePathOf(entry, change.path);
        if (!oldRemotePath || !newRemotePath)
        {
            Log::warn(
                "FileTracking: not mirroring the move in instance {}: {}",
                entry.instanceId.value(),
                (oldRemotePath ? newRemotePath.error() : oldRemotePath.error()).toString()
            );
            return;
        }

        const auto meaning =
            FileTracking::meaningOfMove(entry.serverContent.has(change.path), entry.serverContent.has(oldPath));
        if (meaning == FileTracking::MoveMeaning::Rename)
        {
            if (!entry.moveRemoteOnLocalMove || !operationQueue)
                return;
            operationQueue->enqueueRename(
                *oldRemotePath,
                *newRemotePath,
                [instanceId = entry.instanceId](std::optional<Ids::OperationId> const& operationId, std::string const& info)
                {
                    if (!operationId)
                        Log::error(
                            "FileTracking: failed to enqueue remote rename for instance {}: {}", instanceId.value(), info
                        );
                },
                SharedData::OperationMode::PriorityQueued
            );
            entry.serverContent.move(oldPath, change.path);
            return;
        }

        Log::info("FileTracking: '{}' replaced '{}', uploading it.", oldPath.toUtf8(), change.path.toUtf8());
        if (entry.moveRemoteOnLocalMove && operationQueue && !entry.pendingUploads.removeRemoteWhenDone(oldPath) &&
            entry.serverContent.has(oldPath) && !entry.serverContent.hasBelow(oldPath))
            removeRemote(entry, oldPath, *oldRemotePath);
        onContentChanged(entry, change);
    }

    void removeRemote(TrackedEntry& entry, Paths::LocalPath const& local, Paths::RemotePath const& remote)
    {
        if (!operationQueue)
            return;
        Log::info(
            "FileTracking: enqueueing remote delete for instance {} due to local deletion: {}",
            entry.instanceId.value(),
            remote.value()
        );
        operationQueue->enqueueDelete(
            {remote},
            /*recursive=*/false,
            [instanceId = entry.instanceId](
                std::optional<std::vector<Ids::OperationId>> const& operationIds, std::string const& info
            )
            {
                if (!operationIds)
                    Log::error(
                        "FileTracking: failed to enqueue remote delete for instance {}: {}", instanceId.value(), info
                    );
            },
            SharedData::OperationMode::PriorityQueued
        );
        entry.serverContent.forget(local);
    }

    static void showUploading(TrackedEntry& entry)
    {
        *entry.uploading = !entry.pendingUploads.empty();
    }

    /**
     * @brief Uploads a change unless the same path, or a directory above it, is already on its way; the change then
     *        goes once that upload ended.
     */
    void queueUpload(Upload upload)
    {
        auto* entry = findEntry(upload.instanceId);
        if (!entry || !operationQueue || !entry->pendingUploads.start(upload.change))
            return;
        showUploading(*entry);
        checkServerThenUpload(std::move(upload));
    }

    void checkServerThenUpload(Upload upload)
    {
        const auto* entry = findEntry(upload.instanceId);
        const auto fileEngine = operationQueue ? operationQueue->fileEngine() : nullptr;
        if (!entry || !entry->checkRemoteChanges || !upload.checkServer || !fileEngine)
            return enqueueUpload(std::move(upload));

        fileEngine->stat(
            std::filesystem::path{upload.destination.value()},
            lifetime.guarded(
                [this, upload](
                    std::optional<std::pair<bool, SharedData::DirectoryEntry>> const& result, std::string const& info
                )
                {
                    const auto* entry = findEntry(upload.instanceId);
                    if (!entry)
                        return;
                    if (!result)
                        Log::warn("FileTracking: cannot check '{}' on the server: {}", upload.destination.value(), info);
                    // An unanswered check counts as changed.
                    const bool changed = !result || [&]()
                    {
                        auto const& [exists, stat] = *result;
                        if (upload.change.isDirectory)
                            return exists && !entry->serverContent.has(upload.change.path);
                        return FileTracking::serverChanged(
                            entry->serverContent.fileState(upload.change.path),
                            exists ? std::optional{FileTracking::ServerState{.mtime = stat.mtime, .size = stat.size}}
                                   : std::nullopt
                        );
                    }();
                    if (changed)
                        askServerChanged(upload);
                    else
                        enqueueUpload(upload);
                    syncView();
                }
            )
        );
    }

    void askServerChanged(Upload upload)
    {
        serverChangedQuestions.push_back(std::move(upload));
        if (!serverChangedQuestionOpen)
            askNextServerChanged();
    }

    void askNextServerChanged()
    {
        while (!serverChangedQuestions.empty())
        {
            auto upload = std::move(serverChangedQuestions.front());
            serverChangedQuestions.pop_front();
            if (!findEntry(upload.instanceId))
                continue;

            serverChangedQuestionOpen = true;
            confirmDialog->open({
                .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                .headerText = language->get("fileTracking", "serverChangedHeader"),
                .text = fmt::format(
                    fmt::runtime(language->get("fileTracking", "serverChangedText")), upload.destination.value()
                ),
                .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
                .focusButton = ConfirmDialog::Button::No,
                .onClose = lifetime.guarded(
                    [this, upload](std::optional<ConfirmDialog::Button> button)
                    {
                        serverChangedQuestionOpen = false;
                        if (!button)
                            return abandonServerChangedQuestions(upload);
                        if (*button == ConfirmDialog::Button::Yes)
                            enqueueUpload(upload);
                        else
                        {
                            Log::info("FileTracking: kept the server's version of '{}'.", upload.destination.value());
                            declineUpload(upload);
                        }
                        askNextServerChanged();
                    }
                ),
            });
            return;
        }
    }

    /**
     * @brief Another dialog replaced the question for @p upload: it and the waiting questions are asked again on the
     *        next save, nothing opens now over the dialog that replaced it.
     */
    void abandonServerChangedQuestions(Upload const& upload)
    {
        Log::info("FileTracking: the question about '{}' was replaced by another dialog.", upload.destination.value());
        declineUpload(upload);
        for (auto const& waiting : std::exchange(serverChangedQuestions, {}))
            declineUpload(waiting);
    }

    /**
     * @brief The change is not uploaded, and the changes that arrived while it was asked about wait for the next
     *        save.
     */
    void declineUpload(Upload const& upload)
    {
        if (auto* entry = findEntry(upload.instanceId))
            entry->pendingUploads.changesHandled(upload.change.path);
        finishUpload(upload, UploadOutcome::NotUploaded);
    }

    void enqueueUpload(Upload upload)
    {
        auto* entry = findEntry(upload.instanceId);
        if (!entry || !operationQueue)
            return finishUpload(upload, UploadOutcome::NotUploaded);
        // The upload reads the file when it runs, the changes until now are in it.
        entry->pendingUploads.changesHandled(upload.change.path);
        // allowOverwrite: the tracked file replaces its remote original, checked above.
        operationQueue->enqueueUpload(
            upload.change.path,
            upload.destination,
            /*sizeBytes=*/0,
            lifetime.guarded(
                [this, upload](std::optional<Ids::OperationId> const& operationId, std::string const& info)
                {
                    if (!operationId || !operationQueue)
                    {
                        // A temporary file an editor already renamed or removed ends up here.
                        Log::warn("FileTracking: did not upload '{}': {}", upload.change.path.toUtf8(), info);
                        finishUpload(upload, UploadOutcome::NotUploaded);
                        syncView();
                        return;
                    }
                    operationQueue->addCompletionCallback(
                        *operationId,
                        lifetime.guarded(
                            [this, upload](bool success)
                            {
                                onUploadEnded(upload, success);
                            }
                        )
                    );
                }
            ),
            /*allowOverwrite=*/true,
            /*insertRefresh=*/false,
            /*createMissingDirectories=*/false,
            SharedData::OperationMode::PriorityQueued
        );
    }

    /**
     * @brief Once an upload ended, what the server has now is the state the next check compares with. The upload
     *        stays on its way until that state is known.
     */
    void onUploadEnded(Upload const& upload, bool success)
    {
        if (!success)
            return finishUpload(upload, UploadOutcome::NotUploaded);

        // Uploads carry the local modification time, so the local states are what the server has now.
        if (upload.change.isDirectory)
            return requestFileStates(
                upload.instanceId,
                upload.change.path,
                [this, upload]()
                {
                    finishUpload(upload, UploadOutcome::Uploaded);
                    syncView();
                }
            );

        const auto fileEngine = operationQueue ? operationQueue->fileEngine() : nullptr;
        if (!fileEngine)
            return finishUpload(upload, UploadOutcome::Uploaded);
        fileEngine->stat(
            std::filesystem::path{upload.destination.value()},
            lifetime.guarded(
                [this, upload](
                    std::optional<std::pair<bool, SharedData::DirectoryEntry>> const& result, std::string const&
                )
                {
                    if (auto* entry = findEntry(upload.instanceId); entry && result && result->first)
                        entry->serverContent.rememberFile(
                            upload.change.path,
                            FileTracking::ServerState{.mtime = result->second.mtime, .size = result->second.size}
                        );
                    finishUpload(upload, UploadOutcome::Uploaded);
                    syncView();
                }
            )
        );
    }

    /**
     * @brief The upload of @p upload is no longer on its way. What changed meanwhile goes now: the path itself
     *        once more, the changes below an uploaded directory, or the removal of a path that went away.
     */
    void finishUpload(Upload const& upload, UploadOutcome outcome)
    {
        auto* entry = findEntry(upload.instanceId);
        if (!entry)
            return;
        auto finished = entry->pendingUploads.finish(upload.change.path);
        showUploading(*entry);
        if (finished.removeRemote)
        {
            if (outcome == UploadOutcome::Uploaded || entry->serverContent.has(upload.change.path))
                removeRemote(*entry, upload.change.path, upload.destination);
            return;
        }
        if (finished.changedAgain)
            queueUpload(upload);
        for (auto const& change : finished.changedBelow)
            onContentChanged(*entry, change, /*checkServer=*/false);
    }
};

FileTrackingPanel::FileTrackingPanel(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    ConfirmDialog* confirmDialog
)
    : impl_(std::make_unique<Implementation>(stateHolder, events, confirmDialog))
{}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(FileTrackingPanel);

void FileTrackingPanel::activate(
    OperationQueue* operationQueue,
    Ids::SessionId sessionId,
    std::string downloadTempSuffix
)
{
    impl_->operationQueue = operationQueue;
    impl_->sessionId = std::move(sessionId);
    impl_->downloadTempSuffix = std::move(downloadTempSuffix);
    impl_->refreshOrphanedCount();
}

std::string const& FileTrackingPanel::downloadTempSuffix() const
{
    return impl_->downloadTempSuffix;
}

void FileTrackingPanel::deactivate()
{
    impl_->fileChangeListeners.clear();
    impl_->table->clear();
    impl_->entries.clear();
    impl_->entryCount = std::size_t{0};
    impl_->serverChangedQuestions.clear();
    impl_->operationQueue = nullptr;
    impl_->sessionId = {};
    impl_->orphanedCount = -1;
    impl_->cleanupRunning = false;
}

void FileTrackingPanel::startWatching(
    Ids::InstanceId const& instanceId,
    Paths::LocalPath const& instanceDirectory,
    Paths::RemotePath const& remotePath,
    Paths::LocalPath const& localPath
)
{
    Log::info(
        "FileTracking: starting watch for instance {} (remote: {}, local: {})",
        instanceId.value(),
        remotePath.value(),
        localPath.toUtf8()
    );

    impl_->fileChangeListeners.insert_or_assign(
        instanceId,
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("FileTracking::{}::onFileChanged", instanceId.value()),
            impl_->lifetime.guarded(
                [implementation = impl_.get(), instanceId](Nui::val payload)
                {
                    implementation->onFileChanged(instanceId, payload);
                    Implementation::syncView();
                }
            )
        )
    );

    auto const& options = impl_->stateHolder->stateCache().fileTrackingOptions;
    const auto [entry, inserted] = impl_->entries.insert_or_assign(
        instanceId,
        Implementation::TrackedEntry{
            .instanceId = instanceId,
            .instanceDirectory = instanceDirectory,
            .remotePath = remotePath,
            .localPath = localPath,
            .autoReupload = options.autoReupload,
            .moveRemoteOnLocalMove = options.moveRemoteOnLocalMove,
            .deleteRemote = options.deleteRemoteOnLocalDelete,
            .checkRemoteChanges = options.checkRemoteChanges.value_or(true),
        }
    );
    if (inserted)
    {
        impl_->table->addRow(impl_->buildRowForEntry(entry->second));
        impl_->rows.add(instanceId);
    }
    impl_->entryCount = impl_->entries.size();
    Implementation::syncView();

    // The downloaded files carry the server's modification time, so they tell what the server had.
    impl_->requestFileStates(instanceId, std::nullopt, [] {});
}

Nui::ElementRenderer FileTrackingPanel::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{
        class_ = "file-tracking-panel",
        style = "width: 100%; height: 100%;",
    }(
        impl_->renderHeader(),
        (*impl_->table)({})
    );
    // clang-format on
}
