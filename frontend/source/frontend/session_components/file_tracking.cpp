#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/operation_queue.hpp>

#include <nui-file-explorer/item.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/resizeable_table.hpp>
#include <script-nui-components/button.hpp>
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
    std::filesystem::path instanceDir;
    std::filesystem::path remotePath;
    std::filesystem::path localPath;
    bool autoReupload{true};
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

    Nui::Observed<std::vector<TrackedEntry>> entries{};
    std::unordered_map<std::string, Nui::RpcClient::AutoUnregister> fileChangeListeners{};

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

    ScriptNuiComponents::ResizableTable::TableRow buildRowForEntry(TrackedEntry const& entry)
    {
        using namespace ScriptNuiComponents;
        std::string instanceIdStr = entry.instanceId.value();
        auto uploadingObs = entry.uploading;

        return ResizableTable::TableRow{
            entry.localPath.filename().generic_string(),
            entry.remotePath.generic_string(),
            ResizableTable::TableCell{
                [this, instanceIdStr, isChecked = entry.autoReupload](
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
                                    ent.autoReupload = checked;
                                    break;
                                }
                            }
                        },
                    });
                },
            },
            ResizableTable::TableCell{
                [this, instanceIdStr, isChecked = entry.deleteRemote](
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
                    }(
                        Nui::observe(*uploadingObs).generate([](bool isUploading) -> Nui::ElementRenderer
                        {
                            if (isUploading)
                                return Nui::Elements::text{
                                    std::string{language->get("fileTracking", "statusUploading")}}();
                            return Nui::Elements::text{
                                std::string{language->get("fileTracking", "statusWatching")}}();
                        })
                    );
                },
            },
            ResizableTable::TableCell{
                [this, instanceIdStr](std::unique_ptr<ResizableTable::ISelfController> controller)
                    -> Nui::ElementRenderer
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

void FileTrackingPanel::activate(OperationQueue* operationQueue, Ids::SessionId sessionId)
{
    impl_->operationQueue = operationQueue;
    impl_->sessionId = std::move(sessionId);
}

void FileTrackingPanel::deactivate()
{
    impl_->fileChangeListeners.clear();
    impl_->table->clear();
    impl_->entries.value().clear();
    impl_->operationQueue = nullptr;
    impl_->sessionId = {};
}

void FileTrackingPanel::startWatching(
    Ids::InstanceId const& instanceId,
    std::filesystem::path const& instanceDir,
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath
)
{
    Log::info(
        "FileTracking: starting watch for instance {} (remote: {}, local: {})",
        instanceId.value(),
        remotePath.generic_string(),
        localPath.generic_string()
    );

    impl_->fileChangeListeners[instanceId.value()] = Nui::RpcClient::autoRegisterFunction(
        fmt::format("FileTracking::{}::onFileChanged", instanceId.value()),
        [this, instanceId, remotePath, localPath, instanceDir](Nui::val payload)
        {
            const auto action = payload["action"].as<std::string>();
            const auto directory = payload["directory"].as<std::string>();
            const auto filename = payload["filename"].as<std::string>();

            Log::debug("FileTracking: file changed in instance {}: {} {}", instanceId.value(), action, filename);

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
                    std::filesystem::path changedLocalPath = std::filesystem::path{directory} / filename;
                    std::filesystem::path relPath = changedLocalPath.lexically_relative(instanceDir);
                    std::filesystem::path changedRemotePath = remotePath.parent_path() / relPath;

                    impl_->operationQueue->enqueueDelete(
                        {changedRemotePath},
                        true,
                        [instanceId](std::optional<std::vector<Ids::OperationId>> const& opIds, std::string const& info)
                        {
                            if (!opIds)
                                Log::error(
                                    "FileTracking: failed to enqueue remote delete for instance {}: {}",
                                    instanceId.value(),
                                    info
                                );
                        }
                    );
                }
                return;
            }

            if (action == "Moved")
            {
                if (!impl_->operationQueue)
                    return;

                const auto& oldFilename = payload["oldFilename"].as<std::string>();

                std::filesystem::path oldLocalPath = std::filesystem::path{directory} / oldFilename;
                std::filesystem::path newLocalPath = std::filesystem::path{directory} / filename;

                std::filesystem::path oldRelPath = oldLocalPath.lexically_relative(instanceDir);
                std::filesystem::path newRelPath = newLocalPath.lexically_relative(instanceDir);

                std::filesystem::path oldRemotePath = remotePath.parent_path() / oldRelPath;
                std::filesystem::path newRemotePath = remotePath.parent_path() / newRelPath;

                impl_->operationQueue->enqueueRename(
                    oldRemotePath,
                    newRemotePath,
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

            (*it->uploading) = true;

            std::filesystem::path changedLocalPath = std::filesystem::path{directory} / filename;
            std::filesystem::path relPath = changedLocalPath.lexically_relative(instanceDir);
            std::filesystem::path changedRemotePath = remotePath.parent_path() / relPath;

            NuiFileExplorer::Item localItem{SharedData::DirectoryEntry{
                .path = changedLocalPath,
                .type = SharedData::FileType::Regular,
            }};
            NuiFileExplorer::Item remoteItem{SharedData::DirectoryEntry{
                .path = changedRemotePath,
                .type = SharedData::FileType::Regular,
            }};

            impl_->operationQueue->enqueueUpload(
                remoteItem,
                localItem,
                [this, instanceId](std::optional<Ids::OperationId> const& opId, std::string const& info)
                {
                    if (!opId)
                    {
                        Log::error(
                            "FileTracking: failed to enqueue upload for instance {}: {}", instanceId.value(), info
                        );
                    }
                    auto& vec = impl_->entries.value();
                    for (auto& ent : vec)
                    {
                        if (ent.instanceId.value() == instanceId.value())
                        {
                            (*ent.uploading) = false;
                            break;
                        }
                    }
                },
                true, // allowOverwrite — re-upload always overwrites
                false, // insertRefresh
                SharedData::OperationMode::PriorityQueued
            );
        }
    );

    impl_->entries.value().push_back(
        TrackedEntry{
            .instanceId = instanceId,
            .instanceDir = instanceDir,
            .remotePath = remotePath,
            .localPath = localPath,
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
        (*impl_->table)({})
    );
    // clang-format on
}
