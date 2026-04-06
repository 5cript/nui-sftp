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
    bool uploading{false};
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

    Nui::ListenRemover<Nui::Observed<std::vector<TrackedEntry>>> entriesListener{};

    Implementation(Persistence::StateHolder* stateHolder, FrontendEvents* events, ConfirmDialog* confirmDialog)
        : stateHolder{stateHolder}
        , events{events}
        , confirmDialog{confirmDialog}
    {}

    std::vector<ScriptNuiComponents::ResizableTable::TableRow>
    buildTableRows(std::vector<TrackedEntry> const& trackedEntries)
    {
        using namespace ScriptNuiComponents;
        std::vector<ResizableTable::TableRow> rows;
        rows.reserve(trackedEntries.size());
        for (auto const& entry : trackedEntries)
        {
            std::string instanceIdStr = entry.instanceId.value();
            std::string statusStr = entry.uploading ? std::string{language->get("fileTracking", "statusUploading")}
                                                    : std::string{language->get("fileTracking", "statusWatching")};

            rows.push_back(
                ResizableTable::TableRow{
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
                                    entries.modifyNow();
                                },
                            });
                        },
                    },
                    statusStr,
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
                }
            );
        }
        return rows;
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
{
    impl_->entriesListener = Nui::smartListen(
        impl_->entries,
        [this](std::vector<TrackedEntry> const& vec)
        {
            impl_->table->setRows(impl_->buildTableRows(vec));
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(FileTrackingPanel);

void FileTrackingPanel::activate(OperationQueue* operationQueue, Ids::SessionId sessionId)
{
    impl_->operationQueue = operationQueue;
    impl_->sessionId = std::move(sessionId);
}

void FileTrackingPanel::deactivate()
{
    impl_->fileChangeListeners.clear();
    impl_->entries.value().clear();
    impl_->entries.modifyNow();
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

            if (!it->autoReupload || it->uploading)
                return;

            if (action == "Deleted")
                return; // TODO remove from tracking

            if (!impl_->operationQueue)
            {
                it->uploading = false;
                impl_->entries.modifyNow();
                return;
            }

            it->uploading = true;
            impl_->entries.modifyNow();

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
                            ent.uploading = false;
                            break;
                        }
                    }
                    impl_->entries.modifyNow();
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
