#include <frontend/session_components/operation_queue.hpp>
#include <frontend/observed_random_access_map.hpp>
#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/state_holder_with_dialog.hpp>

#include <frontend/session_components/operation_queue/displayed_transfer_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_scan_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_bulk_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_delete_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_rename_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_operation.hpp>

#include <log/log.hpp>
#include <utility/language.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/svg.hpp>
#include <nui/rpc.hpp>
#include <fmt/format.h>

#include <script-nui-components/button.hpp>
#include <script-nui-components/switch.hpp>

#include <chrono>
#include <functional>
#include <string_view>
#include <unordered_map>

using namespace std::chrono_literals;

struct OperationQueue::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    std::string persistenceSessionName;
    Ids::SessionId sessionId;
    ConfirmDialog* confirmDialog;
    std::shared_ptr<FileEngine> fileEngine;
    LocalSideModel* localModel;
    RemoteSideModel* remoteModel;

    std::vector<Nui::RpcClient::AutoUnregister> onUpdate;
    Nui::Observed<std::string> pausedText{language->get("operationQueue", "continue")};
    Nui::Observed<bool> paused{true};
    std::shared_ptr<Nui::Observed<bool>> autoClean{std::make_shared<Nui::Observed<bool>>(false)};
    ObservedRandomAccessMap<Ids::OperationId, DisplayedOperation, std::map> priorityOperations;
    ObservedRandomAccessMap<Ids::OperationId, DisplayedOperation, std::map> operations;
    Nui::TimerHandle autoCleanTimer;
    std::unordered_map<std::string, std::function<void(bool)>> completionCallbacks;
    Nui::ListenRemover<decltype(paused)> pausedListener{};

    DisplayedOperation* findOperation(Ids::OperationId const& id)
    {
        auto* op = priorityOperations.at(id);
        if (op)
            return op;
        return operations.at(id);
    }

    void eraseOperation(Ids::OperationId const& id)
    {
        if (priorityOperations.at(id))
            priorityOperations.erase(id);
        else
            operations.erase(id);
    }

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::string persistenceSessionName,
        ConfirmDialog* confirmDialog,
        LocalSideModel* localModel,
        RemoteSideModel* remoteModel
    )
        : stateHolder{stateHolder}
        , events{events}
        , persistenceSessionName{std::move(persistenceSessionName)}
        , sessionId{}
        , confirmDialog{confirmDialog}
        , fileEngine{nullptr}
        , localModel{localModel}
        , remoteModel{remoteModel}
        , onUpdate{}
        , operations{}
        , autoCleanTimer{}
    {
        pausedListener = Nui::smartListen(
            paused,
            [this](bool paused)
            {
                this->paused.eventContext().delayToAfterProcessing(
                    [this, paused]()
                    {
                        pausedText = paused ? language->get("operationQueue", "continue")
                                            : language->get("operationQueue", "pause");
                        pausedText.eventContext().sync();
                    }
                );
            }
        );
    }
};

OperationQueue::OperationQueue(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    std::string persistenceSessionName,
    ConfirmDialog* confirmDialog,
    LocalSideModel* localModel,
    RemoteSideModel* remoteModel
)
    : impl_{
          std::make_unique<Implementation>(
              stateHolder,
              events,
              std::move(persistenceSessionName),
              confirmDialog,
              localModel,
              remoteModel
          ),
      }
{
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, name = impl_->persistenceSessionName](bool success, Persistence::State const& state)
        {
            if (!success)
                return;

            auto iter = state.sessions.find(name);
            if (iter == end(state.sessions))
            {
                Log::error("No options found for name: {}", name);
                return;
            }

            auto [engineKey, engine] = *iter;
            impl_->paused = engine.queueOptions->startInPausedState.value_or(true);
            *impl_->autoClean = engine.queueOptions->autoRemoveCompletedOperations.value_or(false);
            Nui::globalEventContext.executeActiveEventsImmediately();
        },
        "Cannot set up operation queue."
    );

    Nui::setInterval(
        1000,
        [this]()
        {
            if (impl_->autoClean->value() && (!impl_->operations.empty() || !impl_->priorityOperations.empty()))
            {
                auto now = std::chrono::steady_clock::now();
                bool anyRemoved = false;

                auto cleanFront = [&](auto& queue)
                {
                    for (auto* front = queue.front(); front != nullptr; front = queue.front())
                    {
                        if (front->isCompletedState())
                        {
                            auto duration = now - front->completionTime();
                            if (duration >= autoRemoveTime)
                            {
                                queue.pop_front();
                                anyRemoved = true;
                                continue;
                            }
                        }
                        break;
                    }
                };

                cleanFront(impl_->priorityOperations);
                cleanFront(impl_->operations);

                if (anyRemoved)
                    Nui::globalEventContext.executeActiveEventsImmediately();
            }
        },
        [this](Nui::TimerHandle&& handle)
        {
            impl_->autoCleanTimer = std::move(handle);
        }
    );
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(OperationQueue);

void OperationQueue::cancelAll()
{
    impl_->operations.clear();
    impl_->priorityOperations.clear();
    Nui::globalEventContext.executeActiveEventsImmediately();
}

template <typename OperationCard>
void OperationQueue::cancelOperation(OperationCard const& operation)
{
    if (operation.isCompletedState())
    {
        const auto id = operation.operationId();
        impl_->eraseOperation(id);
        Nui::globalEventContext.executeActiveEventsImmediately();
        return;
    }

    auto doCancel = [this, operationId = operation.operationId()]()
    {
        Nui::RpcClient::callWithBackChannel(
            fmt::format("OperationQueue::{}::cancel", impl_->sessionId.value()),
            [this, operationId](SharedData::ErrorOrSuccess<> const& result)
            {
                if (!result)
                {
                    return Log::error(
                        "Failed to cancel operation id {}: {}", operationId.value(), result.error.value()
                    );
                }

                auto* operation = impl_->findOperation(operationId);
                if (operation)
                    operation->state(SharedData::OperationState::Canceled);
                Nui::globalEventContext.executeActiveEventsImmediately();
            },
            operationId
        );
    };

    if (operation.warrantsCancelConfirm())
    {
        impl_->confirmDialog->open({
            .headerText = "Cancel Operation",
            .text = fmt::format("Are you sure you want to cancel the operation?"),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .onClose = [doCancel](std::optional<ConfirmDialog::Button> optButton)
            {
                if (optButton && *optButton == ConfirmDialog::Button::Yes)
                    doCancel();
            },
        });
    }
    else
    {
        doCancel();
    }
}

void OperationQueue::deactivate()
{
    impl_->fileEngine.reset();
    impl_->sessionId = {};
}

void OperationQueue::activate(std::shared_ptr<FileEngine> fileEngine, Ids::SessionId sessionId)
{
    impl_->fileEngine = std::move(fileEngine);
    impl_->sessionId = std::move(sessionId);

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onOperationAdded", impl_->sessionId.value()),
            [this](SharedData::OperationAdded const& added)
            {
                onOperationAdded(added);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onDownloadProgress", impl_->sessionId.value()),
            [this](SharedData::TransferProgress const& progress)
            {
                onDownloadProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onUploadProgress", impl_->sessionId.value()),
            [this](SharedData::TransferProgress const& progress)
            {
                onUploadProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onBulkDownloadProgress", impl_->sessionId.value()),
            [this](SharedData::BulkProgress const& progress)
            {
                onBulkDownloadProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onBulkUploadProgress", impl_->sessionId.value()),
            [this](SharedData::BulkProgress const& progress)
            {
                onBulkUploadProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onScanProgress", impl_->sessionId.value()),
            [this](SharedData::ScanProgress const& progress)
            {
                onScanProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onLocalScanProgress", impl_->sessionId.value()),
            [this](SharedData::ScanProgress const& progress)
            {
                onScanProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onDeleteProgress", impl_->sessionId.value()),
            [this](SharedData::BulkDeleteProgress const& result)
            {
                onDeleteProgress(result);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onOperationCompleted", impl_->sessionId.value()),
            [this](Nui::val val)
            {
                onOperationCompleted(std::move(val));
            }
        )
    );

    Nui::RpcClient::callWithBackChannel(
        fmt::format("OperationQueue::{}::isPaused", impl_->sessionId.value()),
        [this](SharedData::ErrorOrSuccess<SharedData::IsPaused> const& result)
        {
            onIsPaused(result);
        }
    );
}

void OperationQueue::onOperationAdded(SharedData::OperationAdded const& added)
{
    auto makeCard = [&added, this]() -> std::unique_ptr<OperationCardInterface>
    {
        std::function<void()> localRefresh;
        std::function<void()> remoteRefresh;
        if (added.insertRefresh)
            localRefresh = [this]()
            {
                impl_->localModel->onRefresh();
            };
        if (added.insertRefresh)
            remoteRefresh = [this]()
            {
                if (impl_->remoteModel)
                    impl_->remoteModel->onRefresh();
            };

        Log::info("Operation of type '{}' added to frontend queue", Utility::enumToString(added.type));
        if (added.type == SharedData::OperationType::Download)
        {
            if (!added.localPath || !added.remotePath)
            {
                Log::error(
                    "Received OperationAdded for operation id: {} without localPath or remotePath",
                    added.operationId.value()
                );
                return {};
            }
            return std::make_unique<DisplayedTransferOperation>(
                added.operationId,
                SharedData::OperationType::Download,
                *impl_->confirmDialog,
                added.totalBytes ? static_cast<long long>(*added.totalBytes) : 0,
                *added.localPath,
                *added.remotePath,
                [this](OperationCard<DisplayedTransferOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                localRefresh
            );
        }
        else if (added.type == SharedData::OperationType::Upload)
        {
            if (!added.localPath || !added.remotePath)
            {
                Log::error(
                    "Received OperationAdded for operation id: {} without localPath or remotePath",
                    added.operationId.value()
                );
                return {};
            }
            return std::make_unique<DisplayedTransferOperation>(
                added.operationId,
                SharedData::OperationType::Upload,
                *impl_->confirmDialog,
                0,
                *added.localPath,
                *added.remotePath,
                [this](OperationCard<DisplayedTransferOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                remoteRefresh
            );
        }
        else if (added.type == SharedData::OperationType::Scan)
        {
            if (!added.remotePath)
            {
                Log::error(
                    "Received OperationAdded for operation id: {} without remotePath", added.operationId.value()
                );
                return {};
            }
            return std::make_unique<DisplayedScanOperation>(
                added.operationId,
                *impl_->confirmDialog,
                *added.remotePath,
                [this](OperationCard<DisplayedScanOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean
            );
        }
        else if (added.type == SharedData::OperationType::LocalScan)
        {
            if (!added.localPath)
            {
                Log::error("Received OperationAdded for operation id: {} without localPath", added.operationId.value());
                return {};
            }
            return std::make_unique<DisplayedScanOperation>(
                added.operationId,
                *impl_->confirmDialog,
                *added.localPath,
                [this](OperationCard<DisplayedScanOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean
            );
        }
        else if (added.type == SharedData::OperationType::Delete)
        {
            Log::info("Creating delete operation card for operation id: {}", added.operationId.value());
            return std::make_unique<DisplayedDeleteOperation>(
                added.operationId,
                *impl_->confirmDialog,
                added.remotePath ? *added.remotePath : std::filesystem::path{},
                [this](OperationCard<DisplayedDeleteOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                remoteRefresh
            );
        }
        else if (added.type == SharedData::OperationType::BulkDownload)
        {
            if (!added.localPath)
            {
                Log::error("Received OperationAdded for operation id: {} without localPath", added.operationId.value());
                return {};
            }
            if (!added.remotePath)
            {
                Log::error(
                    "Received OperationAdded for operation id: {} without remotePath", added.operationId.value()
                );
                return {};
            }
            Log::info("Creating bulk download operation card for operation id: {}", added.operationId.value());
            return std::make_unique<DisplayedBulkOperation>(
                added.operationId,
                *impl_->confirmDialog,
                SharedData::OperationType::BulkDownload,
                *added.localPath,
                *added.remotePath,
                [this](OperationCard<DisplayedBulkOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                localRefresh
            );
        }
        else if (added.type == SharedData::OperationType::BulkUpload)
        {
            if (!added.localPath)
            {
                Log::error("Received OperationAdded for operation id: {} without localPath", added.operationId.value());
                return {};
            }
            if (!added.remotePath)
            {
                Log::error(
                    "Received OperationAdded for operation id: {} without remotePath", added.operationId.value()
                );
                return {};
            }
            Log::info("Creating bulk upload operation card for operation id: {}", added.operationId.value());
            return std::make_unique<DisplayedBulkOperation>(
                added.operationId,
                *impl_->confirmDialog,
                SharedData::OperationType::BulkUpload,
                *added.localPath,
                *added.remotePath,
                [this](OperationCard<DisplayedBulkOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                remoteRefresh
            );
        }
        else if (added.type == SharedData::OperationType::Rename)
        {
            if (!added.localPath || !added.remotePath)
            {
                Log::error(
                    "Received OperationAdded for Rename id: {} without source or destination", added.operationId.value()
                );
                return {};
            }
            return std::make_unique<DisplayedRenameOperation>(
                added.operationId,
                *impl_->confirmDialog,
                *added.localPath,
                *added.remotePath,
                [this](OperationCard<DisplayedRenameOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                remoteRefresh
            );
        }
        Log::error(
            "Received OperationAdded for operation id: {} with unknown type: {}",
            added.operationId.value(),
            static_cast<int>(added.type)
        );
        return {};
    };

    auto card = makeCard();
    if (!card)
    {
        Log::error("Failed to create operation card for operation id: {}", added.operationId.value());
        return;
    }
    Log::info("Inserting operation id: {} into operation queue", added.operationId.value());
    try
    {
        if (added.mode == SharedData::OperationMode::PriorityQueued)
            impl_->priorityOperations.insert(
                added.operationId, DisplayedOperation{added.operationId, added.type, std::move(card)}
            );
        else
            impl_->operations.insert(
                added.operationId, DisplayedOperation{added.operationId, added.type, std::move(card)}
            );
    }
    catch (std::exception const& e)
    {
        Log::error("Failed to insert operation id: {} into operation queue: {}", added.operationId.value(), e.what());
        return;
    }
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void OperationQueue::onDeleteProgress(SharedData::BulkDeleteProgress const& progress)
{
    // Log::debug("Received delete progress for operation id: {}.", progress.operationId.value());

    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received delete progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::Delete)
    {
        Log::error("Received delete progress for operation id: {} which is not a delete", progress.operationId.value());
        return;
    }
    auto* renderer = operation->getCardSpecifically<DisplayedDeleteOperation>();
    if (!renderer)
    {
        Log::error(
            "Received delete progress for operation id: {} which has no delete renderer", progress.operationId.value()
        );
        return;
    }
    renderer->setProgress(progress);
}

void OperationQueue::onDownloadProgress(SharedData::TransferProgress const& progress)
{
    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received download progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::Download)
    {
        Log::error(
            "Received download progress for operation id: {} which is not a download", progress.operationId.value()
        );
        return;
    }
    auto* card = operation->getCardSpecifically<DisplayedTransferOperation>();
    if (!card)
    {
        Log::error(
            "Received download progress for operation id: {} which has no download renderer",
            progress.operationId.value()
        );
        return;
    }
    card->setProgress(progress);
}

void OperationQueue::onScanProgress(SharedData::ScanProgress const& progress)
{
    // Log::debug(
    //     "Received scan progress for operation id: {} - totalBytes: {}, currentIndex: {}, totalItems: {}",
    //     progress.operationId.value(),
    //     progress.totalBytes,
    //     progress.currentIndex,
    //     progress.totalScanned
    // );

    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received scan progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() == SharedData::OperationType::Scan)
    {
        auto* renderer = operation->getCardSpecifically<DisplayedScanOperation>();
        if (!renderer)
        {
            Log::error(
                "Received scan progress for operation id: {} which has no scan renderer", progress.operationId.value()
            );
            return;
        }
        renderer->setProgress(progress.totalBytes, progress.currentIndex, progress.totalScanned);
        return;
    }
    else if (operation->type() == SharedData::OperationType::LocalScan)
    {
        auto* renderer = operation->getCardSpecifically<DisplayedScanOperation>();
        if (!renderer)
        {
            Log::error(
                "Received scan progress for operation id: {} which has no scan renderer", progress.operationId.value()
            );
            return;
        }
        renderer->setProgress(progress.totalBytes, progress.currentIndex, progress.totalScanned);
    }
    else
    {
        Log::error("Received scan progress for operation id: {} which is not a scan", progress.operationId.value());
        return;
    }
}

void OperationQueue::onBulkDownloadProgress(SharedData::BulkProgress const& progress)
{
    // Log::debug("Received bulk download progress for operation id: {}.", progress.operationId.value());

    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received bulk download progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::BulkDownload)
    {
        Log::error(
            "Received bulk download progress for operation id: {} which is not a bulk download",
            progress.operationId.value()
        );
        return;
    }
    auto* card = operation->getCardSpecifically<DisplayedBulkOperation>();
    if (!card)
    {
        Log::error(
            "Received bulk download progress for operation id: {} which has no bulk download renderer",
            progress.operationId.value()
        );
        return;
    }
    card->setProgress(progress);
}

void OperationQueue::onUploadProgress(SharedData::TransferProgress const& progress)
{
    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received upload progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::Upload)
    {
        Log::error(
            "Received upload progress for operation id: {} which is not an upload", progress.operationId.value()
        );
        return;
    }
    auto* renderer = operation->getCardSpecifically<DisplayedTransferOperation>();
    if (!renderer)
    {
        Log::error(
            "Received upload progress for operation id: {} which has no upload renderer", progress.operationId.value()
        );
        return;
    }
    renderer->setProgress(progress);
}

void OperationQueue::onBulkUploadProgress(SharedData::BulkProgress const& progress)
{
    // Log::debug("Received bulk upload progress for operation id: {}.", progress.operationId.value());

    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received bulk upload progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::BulkUpload)
    {
        Log::error(
            "Received bulk upload progress for operation id: {} which is not a bulk upload",
            progress.operationId.value()
        );
        return;
    }
    auto* renderer = operation->getCardSpecifically<DisplayedBulkOperation>();
    if (!renderer)
    {
        Log::error(
            "Received bulk upload progress for operation id: {} which has no bulk upload renderer",
            progress.operationId.value()
        );
        return;
    }
    renderer->setProgress(progress);
}

void OperationQueue::onOperationCompleted(Nui::val val)
{
    SharedData::OperationCompleted completed{};
    try
    {
        Nui::WebApi::Console::log(val);
        Nui::convertFromVal(val, completed);
    }
    catch (std::exception const& e)
    {
        Log::error("Failed to convert OperationCompleted: {}", e.what());
        return;
    }

    Log::info("Received operation completed for operation id: {}", completed.operationId.value());
    auto* operation = impl_->findOperation(completed.operationId);
    if (!operation)
    {
        Log::error("Received operation completed for unknown operation id: {}", completed.operationId.value());
        return;
    }

    bool partial = false;
    if (!completed.failedEntries.empty())
    {
        partial = true;
        Log::warn(
            "Operation id: {} has {} failed entries.", completed.operationId.value(), completed.failedEntries.size()
        );
        operation->failedEntries(std::move(completed.failedEntries));
    }
    switch (completed.reason)
    {
        case (SharedData::OperationCompletionReason::Completed):
        {
            if (partial)
                operation->state(SharedData::OperationState::PartialSuccess);
            else
                operation->state(SharedData::OperationState::Completed);
            break;
        }
        case (SharedData::OperationCompletionReason::Canceled):
        {
            operation->state(SharedData::OperationState::Canceled);
            break;
        }
        case (SharedData::OperationCompletionReason::Failed):
        {
            operation->state(SharedData::OperationState::Failed);
            operation->setError(completed.error.value_or(
                SharedData::OperationError{
                    .type = SharedData::OperationErrorType::UnknownError,
                    .sftpError = std::nullopt,
                    .extraInfo = "Unknown Error"
                }
            ));
            break;
        }
        default:
            Log::warn(
                "Received operation completed for operation id: {} with unknown reason: {}",
                completed.operationId.value(),
                static_cast<int>(completed.reason)
            );
    }
    auto cbIt = impl_->completionCallbacks.find(completed.operationId.value());
    if (cbIt != impl_->completionCallbacks.end())
    {
        bool success = completed.reason == SharedData::OperationCompletionReason::Completed;
        cbIt->second(success);
        impl_->completionCallbacks.erase(cbIt);
    }

    Nui::globalEventContext.executeActiveEventsImmediately();
}

void OperationQueue::addCompletionCallback(Ids::OperationId const& opId, std::function<void(bool success)> callback)
{
    impl_->completionCallbacks[opId.value()] = std::move(callback);
}

void OperationQueue::onIsPaused(SharedData::ErrorOrSuccess<SharedData::IsPaused> const& result)
{
    if (result)
    {
        Log::info("Initial pause state of operation queue is: {}", result.paused ? "paused" : "running");
        impl_->paused = result.paused;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
    else
    {
        Log::error("Failed to get initial paused state: {}", result.error.value());
    }
}

void OperationQueue::togglePause()
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("OperationQueue::{}::pauseUnpause", impl_->sessionId.value()),
        [this, requestToPauseUnpauseWas = !impl_->paused.value()](SharedData::ErrorOrSuccess<> const& result)
        {
            if (result)
            {
                Log::info("{} operation queue successfully", requestToPauseUnpauseWas ? "Paused" : "Unpaused");
                impl_->paused = !impl_->paused.value();
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
            else
                Log::error(
                    "Failed to {} operation queue: {}",
                    requestToPauseUnpauseWas ? "pause" : "unpause",
                    result.error.value()
                );
        },
        !impl_->paused.value()
    );
}

void OperationQueue::askBackendToCancelAll()
{
    Nui::RpcClient::callWithBackChannel(
        fmt::format("OperationQueue::{}::cancelAll", impl_->sessionId.value()),
        [this](SharedData::ErrorOrSuccess<> const& result)
        {
            if (result)
            {
                Log::info("Operation queue all was canceled");
                cancelAll();
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
            else
                Log::error("Failed to cancel all operation queue: {}", result.error.value());
        }
    );
}

void OperationQueue::changeAutoClean(bool doClean)
{
    *impl_->autoClean = doClean;
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, name = impl_->persistenceSessionName](bool success, Persistence::State const&)
        {
            if (!success)
                return;

            auto iter = impl_->stateHolder->stateCache().sessions.find(name);
            iter->second.queueOptions->autoRemoveCompletedOperations = impl_->autoClean->value();
            impl_->stateHolder->save(
                [this](std::optional<std::string> const& error)
                {
                    if (error)
                    {
                        impl_->confirmDialog->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = "Error saving state",
                            .text = fmt::format("An error occurred while saving the application state: {}.", *error),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                    }
                }
            );
        }
    );
}

Nui::ElementRenderer OperationQueue::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    auto makeSummaryText = [this]() -> std::string
    {
        return fmt::format(
            fmt::runtime(language->get("operationQueue", "totalOperations")),
            static_cast<int>(
                impl_->operations.observedValues().value().size() +
                impl_->priorityOperations.observedValues().value().size()
            )
        );
    };

    // clang-format off
    return div{
        class_ = "operation-queue",
    }(
        header{
            class_ = "opq-controls"
        }(
            Snc::button({
                .text = impl_->pausedText,
                .icon = [this]() -> Nui::ElementRenderer {
                    return fragment(
                        observe(impl_->paused).generate([](bool paused)
                        {
                            if (paused)
                                return Svgs::play();
                            return Svgs::pause();
                        })
                    );
                }(),
                .attributes = {
                    onClick = [this](){
                        togglePause();
                    }
                },
            }),
            Snc::button({
                .text = language->getObserved("operationQueue", "cancelAll"),
                .attributes = {
                    onClick = [this](){
                        askBackendToCancelAll();
                    },
                    style = "align-self: stretch"
                },
            }),
            div{
                class_ = "opq-summary"
            }(
                observe(impl_->operations.observedValues(), impl_->priorityOperations.observedValues())
                    .generate(makeSummaryText)
            )
        ),
        // Main content
        div{
            class_ = "opq-list"
        }(
            div{class_ = "opq-priority-list"}(
                // TODO: Make after element depend on list length > 0
                Nui::range(impl_->priorityOperations.observedValues())
                    .after(div{class_ = "opq-priority-separator"}()),
                [](auto, auto const& element) -> Nui::ElementRenderer
                {
                    return (*element)();
                }
            ),
            div{class_ = "opq-regular-list"}(
                impl_->operations.observedValues().map([](auto, auto const& element)
                {
                    return (*element)();
                })
            )
        ),
        // Footer
        div{
            class_ = "opq-footer"
        }(
            Snc::switch_({
                .isChecked = impl_->autoClean,
                .onChange = [this](bool doClean, auto const&) {
                    changeAutoClean(doClean);
                },
                .dontUpdateValue = true,
            }),
            div{
                style = "font-size: 14px; color: var(--color);"
            }(language->getObserved("operationQueue", "autoCleanCompleted"))
        )
    );
    // clang-format on
}

void OperationQueue::enqueueDownload(
    NuiFileExplorer::Item const& remoteItem,
    NuiFileExplorer::Item const& localItem,
    std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
    bool allowOverwrite,
    bool insertRefresh,
    SharedData::OperationMode mode
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue download");
        onComplete(std::nullopt, "No file engine set");
        return;
    }

    Log::info(
        "Frontend Operation Queue download: {} -> {}", remoteItem.path.generic_string(), localItem.path.generic_string()
    );
    impl_->fileEngine->addDownload(remoteItem, localItem, std::move(onComplete), allowOverwrite, insertRefresh, mode);
}
void OperationQueue::enqueueUpload(
    NuiFileExplorer::Item const& remoteItem,
    NuiFileExplorer::Item const& localItem,
    std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
    bool allowOverwrite,
    bool insertRefresh,
    SharedData::OperationMode mode
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue upload");
        onComplete(std::nullopt, "No file engine set");
        return;
    }

    Log::info(
        "Frontend Operation Queue upload: {} -> {}", localItem.path.generic_string(), remoteItem.path.generic_string()
    );
    impl_->fileEngine->addUpload(remoteItem, localItem, std::move(onComplete), allowOverwrite, insertRefresh, mode);
}
void OperationQueue::enqueueRename(
    std::filesystem::path const& oldPath,
    std::filesystem::path const& newPath,
    std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
    SharedData::OperationMode mode
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue rename");
        onComplete(std::nullopt, "No file engine set");
        return;
    }

    Log::info("Frontend Operation Queue rename: {} -> {}", oldPath.generic_string(), newPath.generic_string());
    impl_->fileEngine->addRename(oldPath, newPath, std::move(onComplete), mode);
}
void OperationQueue::enqueueDelete(
    std::vector<std::filesystem::path> const& paths,
    bool recursive,
    std::function<void(std::optional<std::vector<Ids::OperationId>> const&, std::string const& info)> onComplete,
    SharedData::OperationMode mode
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue delete");
        onComplete(std::nullopt, "No file engine set");
        return;
    }
    impl_->fileEngine->removeOnQueueUnchecked(paths, recursive, std::move(onComplete), mode);
}