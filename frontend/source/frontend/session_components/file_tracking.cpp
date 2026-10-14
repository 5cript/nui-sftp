#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/operation_queue.hpp>

#include <nui-file-explorer/item.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/resizeable_table.hpp>
#include <script-nui-components/button.hpp>
#include <script-nui-components/message_strip.hpp>
#include <script-nui-components/switch.hpp>
#include <script-nui-components/style_variant.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/rpc.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::string_literals;

struct TrackedEntry
{
    Ids::InstanceId instanceId;
    Paths::LocalPath instanceDirectory;
    Paths::RemotePath remotePath;
    Paths::LocalPath localPath;
    bool autoReupload{true};
    bool moveRemoteOnLocalMove{false};
    bool deleteRemote{false};
    std::shared_ptr<Nui::Observed<bool>> uploading{std::make_shared<Nui::Observed<bool>>(false)};
};

struct FileTrackingPanel::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    ConfirmDialog* confirmDialog;

    OperationQueue* operationQueue{nullptr};
    Ids::SessionId sessionId{};
    std::string downloadTempSuffix{Persistence::defaultTempFileSuffix};

    Nui::Observed<std::vector<TrackedEntry>> entries{};
    std::unordered_map<std::string, Nui::RpcClient::AutoUnregister> fileChangeListeners{};

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

    Implementation(Persistence::StateHolder* stateHolder, FrontendEvents* events, ConfirmDialog* confirmDialog)
        : stateHolder{stateHolder}
        , events{events}
        , confirmDialog{confirmDialog}
    {}

    void refreshOrphanedCount()
    {
        Log::debug("FileTracking: refreshOrphanedCount() — calling FileTracking::countOrphaned");
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::countOrphaned",
            [this](Nui::val response)
            {
                if (!response.hasOwnProperty("success") || !response["success"].as<bool>())
                {
                    Log::error("FileTracking: countOrphaned RPC returned failure or no 'success' field");
                    return;
                }
                const int cnt = response["count"].as<int>();
                Log::debug(
                    "FileTracking: countOrphaned returned count={}, current orphanedCount={}",
                    cnt,
                    orphanedCount.value()
                );
                orphanedCount = cnt;
                Log::debug("FileTracking: orphanedCount set to {}, notifying observers", orphanedCount.value());
                orphanedCount.eventContext().sync();
            }
        );
    }

    void triggerCleanup()
    {
        Log::debug("FileTracking: triggerCleanup() — setting cleanupRunning=true");
        cleanupRunning = true;
        Log::debug("FileTracking: cleanupRunning is now {}", cleanupRunning.value());
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::forceCleanup",
            [this](Nui::val response)
            {
                Log::debug("FileTracking: forceCleanup callback fired");
                if (!response.hasOwnProperty("success"))
                {
                    Log::error("FileTracking: forceCleanup response has no 'success' field");
                    cleanupRunning = false;
                    return;
                }
                const bool ok = response["success"].as<bool>();
                Log::debug("FileTracking: forceCleanup success={}", ok);
                if (ok && response.hasOwnProperty("removed"))
                    Log::info(
                        "FileTracking: forceCleanup removed {} orphaned instance(s)", response["removed"].as<int>()
                    );
                cleanupRunning = false;
                Log::debug("FileTracking: cleanupRunning reset to false");
                if (ok)
                    refreshOrphanedCount();
            }
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
                        Nui::observe(entries).generate([this]() -> Nui::ElementRenderer
                        {
                            return Nui::Elements::text{fmt::format("{}", entries.value().size())}();
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
                            Log::debug("FileTracking: orphanedCount generate() called, value={}", orphanedCount.value());
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
                        Log::debug("FileTracking: cleanupRunning text generate() called, running={}", running);
                        return std::string{language->get("fileTracking", running ? "headerCleanupRunning" : "headerCleanupButton")};
                    }),
                    .attributes =
                        {
                            Nui::Attributes::class_ = "file-tracking-cleanup-btn",
                            Nui::Attributes::onClick =
                                [this](Nui::val)
                            {
                                if (!cleanupRunning.value())
                                    triggerCleanup();
                            },
                            disabled = Nui::observe(cleanupRunning).generate([](bool running) { return running; }),
                        },
                    .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                })
            )
        );
        // clang-format on
    }

    ScriptNuiComponents::ResizableTable::TableRow buildRowForEntry(TrackedEntry const& entry)
    {
        using namespace ScriptNuiComponents;
        std::string instanceIdStr = entry.instanceId.value();
        auto uploadingObs = entry.uploading;

        return ResizableTable::TableRow{
            entry.localPath.filename() ? std::string{entry.localPath.filename()->view()} : entry.localPath.toUtf8(),
            entry.remotePath.value(),
            ResizableTable::TableCell{
                [this, instanceIdStr, isChecked = entry.autoReupload](std::unique_ptr<ResizableTable::ISelfController>)
                    -> Nui::ElementRenderer
                {
                    return ScriptNuiComponents::switch_({
                        .isChecked = isChecked,
                        .onChange = [this, instanceIdStr](bool checked, Nui::WebApi::MouseEvent const&)
                        {
                            for (auto& ent : entries.value())
                            {
                                if (ent.instanceId.value() == instanceIdStr)
                                {
                                    ent.autoReupload = checked;
                                    break;
                                }
                            }
                        },
                    });
                },
            },
            ResizableTable::TableCell{
                [this, instanceIdStr, isChecked = entry.moveRemoteOnLocalMove](
                    std::unique_ptr<ResizableTable::ISelfController>
                ) -> Nui::ElementRenderer
                {
                    return ScriptNuiComponents::switch_({
                        .isChecked = isChecked,
                        .onChange = [this, instanceIdStr](bool checked, Nui::WebApi::MouseEvent const&)
                        {
                            for (auto& ent : entries.value())
                            {
                                if (ent.instanceId.value() == instanceIdStr)
                                {
                                    ent.moveRemoteOnLocalMove = checked;
                                    break;
                                }
                            }
                        },
                    });
                },
            },
            ResizableTable::TableCell{
                [this, instanceIdStr, isChecked = entry.deleteRemote](std::unique_ptr<ResizableTable::ISelfController>)
                    -> Nui::ElementRenderer
                {
                    return ScriptNuiComponents::switch_({
                        .isChecked = isChecked,
                        .onChange = [this, instanceIdStr](bool checked, Nui::WebApi::MouseEvent const&)
                        {
                            for (auto& ent : entries.value())
                            {
                                if (ent.instanceId.value() == instanceIdStr)
                                {
                                    ent.deleteRemote = checked;
                                    break;
                                }
                            }
                        },
                    });
                },
            },
            ResizableTable::TableCell{
                [uploadingObs](std::unique_ptr<ResizableTable::ISelfController>) -> Nui::ElementRenderer
                {
                    return Nui::Elements::div{
                        Nui::Attributes::class_ = "file-tracking-status",
                        Nui::Attributes::style = "display:contents",
                    }(Nui::observe(*uploadingObs)
                            .generate(
                                [](bool isUploading) -> Nui::ElementRenderer
                                {
                                    if (isUploading)
                                        return Nui::Elements::text{
                                            std::string{language->get("fileTracking", "statusUploading")}
                                        }();
                                    return Nui::Elements::text{
                                        std::string{language->get("fileTracking", "statusWatching")}
                                    }();
                                }
                            ));
                },
            },
            ResizableTable::TableCell{
                [this,
                    instanceIdStr](std::unique_ptr<ResizableTable::ISelfController> controller) -> Nui::ElementRenderer
                {
                    std::shared_ptr<ResizableTable::ISelfController> sharedCtrl = std::move(controller);
                    return ScriptNuiComponents::button({
                        .text = std::string{language->get("fileTracking", "stopButton")},
                        .attributes =
                            {
                                Nui::Attributes::onClick =
                                    [this, instanceIdStr, ctrl = std::move(sharedCtrl)](Nui::val)
                                {
                                    stopWatching(instanceIdStr, ctrl.get());
                                },
                            },
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    });
                },
            },
        };
    }

    void stopWatching(std::string const& instanceIdStr, ScriptNuiComponents::ResizableTable::ISelfController* ctrl)
    {
        Log::info("FileTracking: stopping watch for instance {}", instanceIdStr);

        // Pass instanceId as a positional arg to match backend handler signature: (reply, instanceId: string)
        Nui::RpcClient::callWithBackChannel(
            "FileTracking::destroyInstance",
            [this, instanceIdStr, ctrl](Nui::val response)
            {
                if (!response.hasOwnProperty("success") || !response["success"].as<bool>())
                {
                    Log::error("FileTracking: destroyInstance failed for {}", instanceIdStr);
                    return;
                }
                fileChangeListeners.erase(instanceIdStr);
                auto& vec = entries.value();
                vec.erase(
                    std::remove_if(
                        vec.begin(),
                        vec.end(),
                        [&instanceIdStr](TrackedEntry const& ent)
                        {
                            return ent.instanceId.value() == instanceIdStr;
                        }
                    ),
                    vec.end()
                );
                if (ctrl)
                    ctrl->remove();
                entries.modifyNow();
            },
            instanceIdStr
        );
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
    Log::debug("FileTracking: activate() — triggering initial orphaned count refresh");
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
    impl_->entries.value().clear();
    impl_->operationQueue = nullptr;
    impl_->sessionId = {};
    impl_->orphanedCount = -1;
    impl_->cleanupRunning = false;
}

namespace
{
    /**
     * @brief The remote path a change at @p changed maps to. The instance directory stands for the directory of
     *        @p remotePath, a change outside it or of the instance directory itself maps to nothing.
     */
    std::expected<Paths::RemotePath, Paths::PathError> remotePathOfChange(
        Paths::LocalPath const& changed,
        Paths::LocalPath const& instanceDirectory,
        Paths::RemotePath const& remotePath
    )
    {
        const auto remoteDirectory = remotePath.parent();
        if (!remoteDirectory)
            return std::unexpected(Paths::PathError{.reason = Paths::PathRejection::EscapesRoot, .input = remotePath.value()});
        return changed.relativeTo(instanceDirectory)
            .and_then(
                [&changed, &remoteDirectory](
                    Paths::RelativePath const& relative
                ) -> std::expected<Paths::RemotePath, Paths::PathError>
                {
                    if (relative.empty())
                        return std::unexpected(Paths::PathError{.reason = Paths::PathRejection::Empty, .input = changed.toUtf8()});
                    return *remoteDirectory / relative;
                }
            );
    }
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

    impl_->fileChangeListeners[instanceId.value()] = Nui::RpcClient::autoRegisterFunction(
        fmt::format("FileTracking::{}::onFileChanged", instanceId.value()),
        [this, instanceId, remotePath, localPath, instanceDirectory](Nui::val payload)
        {
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
            const auto isTracked = [&localPath](std::expected<Paths::LocalPath, Paths::PathError> const& changed)
            {
                return changed && changed->isWithin(localPath);
            };
            const auto changedLocalPath = localPathOfChange(filename);
            if (!isTracked(changedLocalPath))
            {
                Log::debug("FileTracking: ignoring change outside the tracked item: {}", filename);
                return;
            }

            auto& vec = impl_->entries.value();
            auto it = std::find_if(
                vec.begin(),
                vec.end(),
                [&instanceId](TrackedEntry const& ent)
                {
                    return ent.instanceId.value() == instanceId.value();
                }
            );
            if (it == vec.end())
                return;

            if (action == "Deleted")
            {
                if (it->deleteRemote && impl_->operationQueue)
                {
                    // The deleted item must map to a path below the tracked remote directory, never next to it.
                    const auto remoteOfChange = remotePathOfChange(*changedLocalPath, instanceDirectory, remotePath);
                    if (!remoteOfChange)
                    {
                        Log::error(
                            "FileTracking: not mirroring the deletion of '{}': {}",
                            changedLocalPath->toUtf8(),
                            remoteOfChange.error().toString()
                        );
                        return;
                    }
                    const auto changedRemotePath = *remoteOfChange;

                    Log::info(
                        "FileTracking: enqueueing remote delete for instance {} due to local deletion: {}",
                        instanceId.value(),
                        changedRemotePath.value()
                    );

                    // Never recursive: the event does not say whether a file or a directory went, and a deleted
                    // local file must not take a remote directory of that name with it. A deleted local directory
                    // reports its content first, so its remote copy is empty by the time it is removed; anything
                    // only the server has keeps it.
                    impl_->operationQueue->enqueueDelete(
                        {changedRemotePath},
                        /*recursive=*/false,
                        [instanceId](std::optional<std::vector<Ids::OperationId>> const& opIds, std::string const& info)
                        {
                            if (!opIds)
                                Log::error(
                                    "FileTracking: failed to enqueue remote delete for instance {}: {}",
                                    instanceId.value(),
                                    info
                                );
                        },
                        SharedData::OperationMode::PriorityQueued
                    );
                }
                return;
            }

            const auto oldLocalPath = localPathOfChange(payload["oldFilename"].as<std::string>());
            // An atomic save renames a temporary from outside the tracked item over it, a change of its content.
            if (action == "Moved" && isTracked(oldLocalPath))
            {
                if (!it->moveRemoteOnLocalMove || !impl_->operationQueue)
                    return;

                // Both ends must lie inside the instance directory to avoid arbitrary remote renames.
                auto oldRemotePath = remotePathOfChange(*oldLocalPath, instanceDirectory, remotePath);
                auto newRemotePath = remotePathOfChange(*changedLocalPath, instanceDirectory, remotePath);
                if (!oldRemotePath || !newRemotePath)
                {
                    Log::warn(
                        "FileTracking: not mirroring the move in instance {}: {}",
                        instanceId.value(),
                        (oldRemotePath ? newRemotePath.error() : oldRemotePath.error()).toString()
                    );
                    return;
                }

                impl_->operationQueue->enqueueRename(
                    *oldRemotePath,
                    *newRemotePath,
                    [instanceId](std::optional<Ids::OperationId> const& opId, std::string const& info)
                    {
                        if (!opId)
                            Log::error(
                                "FileTracking: failed to enqueue remote rename for instance {}: {}",
                                instanceId.value(),
                                info
                            );
                    },
                    SharedData::OperationMode::PriorityQueued
                );
                return;
            }

            if (!it->autoReupload || *it->uploading)
                return;

            if (!impl_->operationQueue)
                return;

            const auto source = *changedLocalPath;
            const auto destination = remotePathOfChange(source, instanceDirectory, remotePath);
            if (!destination)
            {
                Log::warn(
                    "FileTracking: not uploading the change in instance {}: {}",
                    instanceId.value(),
                    destination.error().toString()
                );
                return;
            }

            impl_->queueReupload(instanceId, source, *destination);
        }
    );

    auto const& ftOpts = impl_->stateHolder->stateCache().fileTrackingOptions;
    impl_->entries.value().push_back(
        TrackedEntry{
            .instanceId = instanceId,
            .instanceDirectory = instanceDirectory,
            .remotePath = remotePath,
            .localPath = localPath,
            .autoReupload = ftOpts.autoReupload,
            .moveRemoteOnLocalMove = ftOpts.moveRemoteOnLocalMove,
            .deleteRemote = ftOpts.deleteRemoteOnLocalDelete,
        }
    );
    impl_->table->addRow(impl_->buildRowForEntry(impl_->entries.value().back()));
    impl_->entries.modifyNow();
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
