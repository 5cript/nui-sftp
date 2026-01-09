#include <frontend/session_components/operation_queue.hpp>
#include <frontend/observed_random_access_map.hpp>
#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/state_holder_with_dialog.hpp>

#include <frontend/session_components/operation_queue/displayed_download_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_upload_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_scan_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_bulk_download_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_operation.hpp>
#include <frontend/session_components/operation_queue/displayed_custom_action.hpp>

#include <log/log.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/svg.hpp>
#include <nui/rpc.hpp>
#include <fmt/format.h>

#include <ui5/components/switch.hpp>
#include <ui5/components/button.hpp>

#include <chrono>
#include <string_view>

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
    Nui::Observed<bool> paused{true};
    std::shared_ptr<Nui::Observed<bool>> autoClean{std::make_shared<Nui::Observed<bool>>(false)};
    ObservedRandomAccessMap<Ids::OperationId, DisplayedOperation, std::map> operations;
    Nui::TimerHandle autoCleanTimer;

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
    {}
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
            if (impl_->autoClean->value() && !impl_->operations.empty())
            {
                auto now = std::chrono::steady_clock::now();
                bool anyRemoved = false;
                for (auto* front = impl_->operations.front(); front != nullptr; front = impl_->operations.front())
                {
                    if (front->isCompletedState())
                    {
                        auto duration = now - front->completionTime();
                        if (duration >= autoRemoveTime)
                        {
                            impl_->operations.pop_front();
                            anyRemoved = true;
                            continue;
                        }
                    }
                    break;
                }
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

template <typename OperationCard>
void OperationQueue::cancelOperation(OperationCard const& operation)
{
    if (operation.isCompletedState())
    {
        impl_->operations.erase(operation.operationId());
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
    else
    {
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

                    auto* operation = impl_->operations.at(operationId);
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
                .onClose = [doCancel](ConfirmDialog::Button buttonPressed)
                {
                    if (buttonPressed == ConfirmDialog::Button::Yes)
                        doCancel();
                },
            });
        }
        else
        {
            doCancel();
        }
    }
    cleanupCustomActionsAfterId(operation.operationId());
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
            [this](SharedData::DownloadProgress const& progress)
            {
                onDownloadProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onBulkDownloadProgress", impl_->sessionId.value()),
            [this](SharedData::BulkDownloadProgress const& progress)
            {
                onBulkDownloadProgress(progress);
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
            return std::make_unique<DisplayedDownloadOperation>(
                added.totalBytes ? static_cast<long long>(*added.totalBytes) : 0,
                added.operationId,
                *added.localPath,
                *added.remotePath,
                [this](OperationCard<DisplayedDownloadOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean
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
            return std::make_unique<DisplayedUploadOperation>(
                added.totalBytes ? static_cast<long long>(*added.totalBytes) : 0,
                added.operationId,
                *added.localPath,
                *added.remotePath,
                [this](OperationCard<DisplayedUploadOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean
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
                [this](OperationCard<DisplayedScanOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean
            );
        }
        else if (added.type == SharedData::OperationType::BulkDownload)
        {
            Log::info("Creating bulk download operation card for operation id: {}", added.operationId.value());
            return std::make_unique<DisplayedBulkDownloadOperation>(
                added.operationId,
                [this](OperationCard<DisplayedBulkDownloadOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean
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
        impl_->operations.insert(added.operationId, DisplayedOperation{added.operationId, added.type, std::move(card)});
        if (added.insertRefresh)
        {
            if (added.type == SharedData::OperationType::Download)
            {
                addCustomActionOperation(
                    [this]()
                    {
                        impl_->localModel->onRefresh();
                    }
                );
            }
            else if (added.type == SharedData::OperationType::Upload)
            {
                addCustomActionOperation(
                    [this]()
                    {
                        impl_->remoteModel->onRefresh();
                    }
                );
            }
        }
    }
    catch (std::exception const& e)
    {
        Log::error("Failed to insert operation id: {} into operation queue: {}", added.operationId.value(), e.what());
        return;
    }
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void OperationQueue::addCustomActionOperation(std::function<void()> action)
{
    auto id = Ids::generateOperationId();
    impl_->operations.insert(
        id,
        DisplayedOperation{
            id,
            SharedData::OperationType::CustomAction,
            std::make_unique<DisplayedCustomAction>(
                id,
                [this](OperationCard<DisplayedCustomAction> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                [action = std::move(action)](std::optional<Ids::OperationId> const&)
                {
                    if (action)
                        action();
                }
            ),
        }
    );
}

void OperationQueue::onDownloadProgress(SharedData::DownloadProgress const& progress)
{
    Log::debug(
        "Received download progress for operation id: {} - {}/{}",
        progress.operationId.value(),
        progress.current - progress.min,
        progress.max - progress.min
    );

    auto* operation = impl_->operations.at(progress.operationId);
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
    auto* renderer = operation->getCardSpecifically<DisplayedDownloadOperation>();
    if (!renderer)
    {
        Log::error(
            "Received download progress for operation id: {} which has no download renderer",
            progress.operationId.value()
        );
        return;
    }
    renderer->setProgress(progress.current - progress.min);
}

void OperationQueue::onScanProgress(SharedData::ScanProgress const& progress)
{

    Log::debug(
        "Received scan progress for operation id: {} - totalBytes: {}, currentIndex: {}, totalItems: {}",
        progress.operationId.value(),
        progress.totalBytes,
        progress.currentIndex,
        progress.totalScanned
    );

    auto* operation = impl_->operations.at(progress.operationId);
    if (!operation)
    {
        Log::error("Received scan progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::Scan)
    {
        Log::error("Received scan progress for operation id: {} which is not a scan", progress.operationId.value());
        return;
    }
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

void OperationQueue::onBulkDownloadProgress(SharedData::BulkDownloadProgress const& progress)
{
    Log::debug("Received bulk download progress for operation id: {}.", progress.operationId.value());

    auto* operation = impl_->operations.at(progress.operationId);
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
    auto* renderer = operation->getCardSpecifically<DisplayedBulkDownloadOperation>();
    if (!renderer)
    {
        Log::error(
            "Received bulk download progress for operation id: {} which has no bulk download renderer",
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
        Nui::convertFromVal(val, completed);
    }
    catch (std::exception const& e)
    {
        Log::error("Failed to convert OperationCompleted: {}", e.what());
        return;
    }

    Log::info("Received operation completed for operation id: {}", completed.operationId.value());
    auto* operation = impl_->operations.at(completed.operationId);
    if (!operation)
    {
        Log::error("Received operation completed for unknown operation id: {}", completed.operationId.value());
        return;
    }
    switch (completed.reason)
    {
        case (SharedData::OperationCompletionReason::Completed):
        {
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
            break;
        }
        default:
            Log::warn(
                "Received operation completed for operation id: {} with unknown reason: {}",
                completed.operationId.value(),
                static_cast<int>(completed.reason)
            );
    }
    cleanupCustomActionsAfterId(completed.operationId);
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void OperationQueue::cleanupCustomActionsAfterId(Ids::OperationId const& id)
{
    auto iter = impl_->operations.find(id);
    if (iter != impl_->operations.end())
    {
        ++iter;
        for (
            auto end = impl_->operations.end();
            iter != end && iter->get()->type() == SharedData::OperationType::CustomAction;
            ++iter
        )
        {
            iter->get()->state(SharedData::OperationState::Completed);
        }
    }
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

Nui::ElementRenderer OperationQueue::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    auto operationsMapper = [](auto, auto const& element)
    {
        std::cout << "Mapping operation element" << std::endl;
        return div{}((*element)());
    };

    auto makeSummaryText = [this]() -> std::string
    {
        return fmt::format("{} total operations", static_cast<int>(impl_->operations.observedValues().value().size()));
    };

    // clang-format off
    return div{
        class_ = "operation-queue",
    }(
        header{
            class_ = "opq-controls"
        }(
            div{
                class_ = "opq-play-toggle",
                role = "button",
                tabIndex = "0",
                onClick = [this](){
                    Nui::RpcClient::callWithBackChannel(
                        fmt::format("OperationQueue::{}::pauseUnpause", impl_->sessionId.value()),
                        [this, requestToPauseUnpauseWas = !impl_->paused.value()](SharedData::ErrorOrSuccess<> const& result)
                        {
                            if (result) {
                                Log::info("{} operation queue successfully", requestToPauseUnpauseWas ? "Paused" : "Unpaused");
                                impl_->paused = !impl_->paused.value();
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                            else
                                Log::error("Failed to {} operation queue: {}", requestToPauseUnpauseWas ? "pause" : "unpause", result.error.value());
                        },
                        !impl_->paused.value()
                    );
                }
            }(
                div{
                    class_ = "opq-icon",
                }(
                    observe(impl_->paused).generate([this](){
                        if (impl_->paused.value())
                            return Svgs::play();
                        return Svgs::pause();
                    })
                ),
                div{
                    class_ = "opq-label",
                }(
                    observe(impl_->paused).generate([this]() -> std::string {
                        if (!impl_->paused.value())
                            return "Pause";
                        return "Continue";
                    })
                )
            ),
            div{
                class_ = "opq-summary"
            }(
                observe(impl_->operations.observedValues()).generate(makeSummaryText)
            )
        ),
        // Main content
        div{
            class_ = "opq-list"
        }(
            impl_->operations.observedValues().map(operationsMapper)
        ),
        // Footer
        div{
            class_ = "opq-footer"
        }(
            ui5::switch_{
                "checked"_prop = impl_->autoClean,
                "design"_prop = "Graphical",
                "change"_event = [this](Nui::val event) {
                    *impl_->autoClean = event["target"]["checked"].as<bool>();
                    loadState(
                        *impl_->stateHolder,
                        impl_->confirmDialog,
                        [this, name = impl_->persistenceSessionName](bool success, Persistence::State const&) {
                            if (!success)
                                return;

                            auto iter = impl_->stateHolder->stateCache().sessions.find(name);
                            iter->second.queueOptions->autoRemoveCompletedOperations = impl_->autoClean->value();
                            impl_->stateHolder->save([this](std::optional<std::string> const& error) {
                                if (error) {
                                    impl_->confirmDialog->open({
                                        .state = ConfirmDialog::State::Negative,
                                        .headerText = "Error saving state",
                                        .text = fmt::format(
                                            "An error occurred while saving the application state: {}.",
                                            *error
                                        ),
                                        .buttons = ConfirmDialog::Button::Ok,
                                    });
                                }
                            });
                    });
                }
            }(),
            div{
                style = "font-size: 14px; color: var(--muted);"
            }("Auto Remove Completed Operations")
        )
    );
    // clang-format on
}

void OperationQueue::enqueueDownload(
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    std::function<void(std::optional<Ids::OperationId> const&)> onComplete,
    bool allowOverwrite,
    bool insertRefresh
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue download");
        onComplete(std::nullopt);
        return;
    }

    Log::info("Frontend Operation Queue download: {} -> {}", remotePath.generic_string(), localPath.generic_string());
    impl_->fileEngine->addDownload(remotePath, localPath, std::move(onComplete), allowOverwrite, insertRefresh);
}
void OperationQueue::enqueueUpload(
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    std::function<void(std::optional<Ids::OperationId> const&)> onComplete,
    bool allowOverwrite,
    bool insertRefresh
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue upload");
        onComplete(std::nullopt);
        return;
    }

    Log::info("Frontend Operation Queue upload: {} -> {}", localPath.generic_string(), remotePath.generic_string());
    impl_->fileEngine->addUpload(remotePath, localPath, std::move(onComplete), allowOverwrite, insertRefresh);
}
void OperationQueue::enqueueRename(
    std::filesystem::path const&,
    std::filesystem::path const&,
    std::function<void(std::optional<Ids::OperationId> const&)> onComplete
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue download");
        onComplete(std::nullopt);
        return;
    }
    // TODO: Implement
}
void OperationQueue::enqueueDelete(
    std::filesystem::path const&,
    std::function<void(std::optional<Ids::OperationId> const&)> onComplete
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue download");
        onComplete(std::nullopt);
        return;
    }
    // TODO: Implement
}