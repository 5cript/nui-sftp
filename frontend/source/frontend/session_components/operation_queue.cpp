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
#include <constants/sftp.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/api/keyboard_event.hpp>
#include <nui/frontend/api/drag_event.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/svg.hpp>
#include <nui/frontend/api/json.hpp>
#include <nui/rpc.hpp>
#include <fmt/format.h>

#include <nlohmann/json.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/switch.hpp>
#include <script-nui-components/pagination.hpp>

#include <ui5-sap-icons/icons/synchronize.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

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

    // History tier: live-queue elements evicted after the live page overflows.
    // Shared ownership lets the same card also appear in `failed` without
    // risking dangling raw pointers when one container drops it.
    std::deque<std::shared_ptr<DisplayedOperation>> history;
    std::unordered_map<std::string, std::shared_ptr<DisplayedOperation>> historyIndex;

    // Failed copy-list: tracks failed operations so the user can review them
    // via a dedicated tab even after auto-clean removed them from the queue.
    // Shared ownership with live/history means no copy-of-card is required.
    Nui::Observed<std::deque<std::shared_ptr<DisplayedOperation>>> failed;
    std::unordered_map<std::string, std::shared_ptr<DisplayedOperation>> failedIndex;

    // Pagination / navigation state.
    Nui::Observed<int> pageCount{1};
    Nui::Observed<int> currentPage{0}; // pageCount-1 is the live page.
    Nui::Observed<int> activeTab{0}; // 0 = Queue (live + history), 1 = Failed, 2 = Priority
    // True until the user explicitly clicks a non-live history page. While
    // true, the live page follows overflow growth so the user is not yanked
    // into a stale history page when a new eviction bumps pageCount.
    bool followLive{true};
    int liveQueuePageSize{200};

    Nui::TimerHandle autoCleanTimer;
    std::unordered_map<std::string, std::function<void(bool)>> completionCallbacks;
    std::unordered_map<std::string, std::function<void(double)>> transferProgressCallbacks;
    // Keyed by the aggregate bulk operation id (first pre-generated opId for
    // bulk up/download, or the aggregate id for bulk delete).  Auto-erased on
    // completion, same as transferProgressCallbacks.
    std::unordered_map<std::string, std::function<void(SharedData::BulkProgress const&)>> bulkProgressCallbacks;
    // Keyed by operationId.value(); used for per-operation sync scan progress events.
    std::unordered_map<std::string, std::function<void(SharedData::ScanProgress const&)>> syncScanProgressCallbacks;

    // Keyed by syncSessionId.value(); routes the backend's phase-done / diff-progress
    // streams to the provider.  Cleared via @ref clearSyncSessionRouting.
    struct SyncSessionRouting
    {
        std::array<Ids::OperationId, 2> scanIds{};   // [0]=remote, [1]=local
        std::function<void(bool isLocal)> onScanPhaseDone;
        std::function<void(std::uint64_t)> onDiffProgress;
    };
    std::unordered_map<std::string, SyncSessionRouting> syncSessionRouting;
    Nui::ListenRemover<decltype(paused)> pausedListener{};

    // Minimized-sync restore button state.  `minimizedSyncVisible` drives
    // display; `minimizedSyncShine` is bumped each time we (re)minimize the
    // dialog so the Snc::button shine replays.  `minimizedSyncRestore` is the
    // callback invoked when the user clicks the restore button.
    Nui::Observed<bool> minimizedSyncVisible{false};
    Nui::Observed<int> minimizedSyncShine{0};
    std::function<void()> minimizedSyncRestore{};

    // Drag-state (regular-queue reorder).  Held outside any per-card observed
    // so the hover indicator costs zero per-card overhead and avoids extra
    // re-renders.  `currentHoverEl` is the DOM element the drop indicator is
    // currently attached to via direct classList manipulation (mirroring
    // icon_flavor's setHoverItem pattern).
    Nui::val currentHoverEl{Nui::val::null()};
    bool currentHoverBelow{false};

    void clearDragHover()
    {
        if (currentHoverEl.isNull() || currentHoverEl.isUndefined())
            return;
        auto cl = currentHoverEl["classList"];
        cl.call<void>("remove", std::string{"opq-drop-above"});
        cl.call<void>("remove", std::string{"opq-drop-below"});
        currentHoverEl = Nui::val::null();
    }

    void setDragHover(Nui::val targetEl, bool below)
    {
        if (!currentHoverEl.isNull() && !currentHoverEl.isUndefined() &&
            currentHoverEl.equals(targetEl) && currentHoverBelow == below)
            return;
        clearDragHover();
        if (targetEl.isNull() || targetEl.isUndefined())
            return;
        targetEl["classList"].call<void>("add", std::string{below ? "opq-drop-below" : "opq-drop-above"});
        currentHoverEl = std::move(targetEl);
        currentHoverBelow = below;
    }

    DisplayedOperation* findOperation(Ids::OperationId const& id)
    {
        if (auto* op = priorityOperations.at(id))
            return op;
        if (auto* op = operations.at(id))
            return op;
        if (auto it = historyIndex.find(id.value()); it != historyIndex.end())
            return it->second.get();
        if (auto it = failedIndex.find(id.value()); it != failedIndex.end())
            return it->second.get();
        return nullptr;
    }

    /**
     * @brief Remove the operation from every container that currently holds it.
     *        Cascades by id so the Failed copy-list stays consistent with the
     *        live/history tiers without any cross-container pointer references.
     */
    void eraseOperation(Ids::OperationId const& id)
    {
        if (priorityOperations.at(id))
            priorityOperations.erase(id);
        if (operations.at(id))
            operations.erase(id);

        bool historyChanged = false;
        if (auto it = historyIndex.find(id.value()); it != historyIndex.end())
        {
            historyIndex.erase(it);
            std::erase_if(
                history,
                [&](auto const& entry)
                {
                    return entry->key() == id;
                }
            );
            historyChanged = true;
        }
        if (auto it = failedIndex.find(id.value()); it != failedIndex.end())
        {
            failedIndex.erase(it);
            auto& deque = failed.value();
            std::erase_if(
                deque,
                [&](auto const& entry)
                {
                    return entry->key() == id;
                }
            );
            failed.modify();
        }
        if (historyChanged)
            recomputePageCount();
    }

    /**
     * @brief Snapshot an operation into the Failed tab. Uses shared ownership
     *        so the same card continues to render correctly in the Queue tab
     *        (live or history) without requiring a deep copy of the card.
     */
    void recordFailed(Ids::OperationId const& id)
    {
        if (failedIndex.contains(id.value()))
            return;

        std::shared_ptr<DisplayedOperation> entry;
        if (auto raam = priorityOperations.shared(id))
            entry = raam;
        else if (auto raam = operations.shared(id))
            entry = raam;
        else if (auto it = historyIndex.find(id.value()); it != historyIndex.end())
            entry = it->second;
        if (!entry)
            return;

        failedIndex.emplace(id.value(), entry);
        failed.push_back(std::move(entry));
    }

    /**
     * @brief Recompute page count from history size and clamp currentPage.
     *        Live page is always the last page (index pageCount - 1).
     */
    void recomputePageCount()
    {
        const int pageSize = std::max(1, liveQueuePageSize);
        const int historySize = static_cast<int>(history.size());
        const int newPageCount = 1 + (historySize + pageSize - 1) / pageSize;

        // Dedupe Observed assignments: Nui's operator= does NOT compare old and
        // new values, so every identical assignment fires observers. On the
        // eviction-per-insert overflow path this causes the entire .opq-list
        // subtree to re-render on every new op — unacceptable perf.
        if (pageCount.value() != newPageCount)
            pageCount = newPageCount;

        const int desiredPage = followLive
            ? newPageCount - 1
            : std::clamp(currentPage.value(), 0, newPageCount - 1);
        if (currentPage.value() != desiredPage)
            currentPage = desiredPage;
    }

    /**
     * @brief Enforce the configured live-queue page size by moving the oldest
     *        overflow from `operations` to `history` via a single block erase
     *        (Nui emits one range-erase diff rather than N individual diffs).
     */
    void enforceLivePageSize()
    {
        auto& deque = operations.observedValues().value();
        const int overflow = static_cast<int>(deque.size()) - std::max(1, liveQueuePageSize);
        if (overflow <= 0)
            return;

        auto evicted = operations.extract_front_n(static_cast<std::size_t>(overflow));
        for (auto& entry : evicted)
        {
            const auto key = entry->key().value();
            historyIndex.emplace(key, entry);
            history.push_back(std::move(entry));
        }
        recomputePageCount();
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
            impl_->liveQueuePageSize = std::clamp(engine.queueOptions->liveQueuePageSize.value_or(200), 1, 1000);
            impl_->enforceLivePageSize();
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

                // History can contain completed ops (either completed before
                // being evicted or force-popped while still running and later
                // finished). Sweep any whose completion window has elapsed.
                const auto sizeBefore = impl_->history.size();
                std::erase_if(
                    impl_->history,
                    [&](auto const& entry)
                    {
                        if (!entry->isCompletedState())
                            return false;
                        if (now - entry->completionTime() < autoRemoveTime)
                            return false;
                        impl_->historyIndex.erase(entry->key().value());
                        return true;
                    }
                );
                if (impl_->history.size() != sizeBefore)
                {
                    impl_->recomputePageCount();
                    anyRemoved = true;
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

void OperationQueue::cancelAll()
{
    impl_->operations.clear();
    impl_->priorityOperations.clear();
    impl_->history.clear();
    impl_->historyIndex.clear();
    // Failed list survives "Cancel All": it's a retrospective review tool,
    // not a live-state container. Users can still clear it explicitly.
    impl_->recomputePageCount();
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
            fmt::format("OperationQueue::{}::onArchiveDownloadProgress", impl_->sessionId.value()),
            [this](SharedData::TransferProgress const& progress)
            {
                onArchiveDownloadProgress(progress);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onArchiveUploadProgress", impl_->sessionId.value()),
            [this](SharedData::TransferProgress const& progress)
            {
                onArchiveUploadProgress(progress);
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

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onOperationsReordered", impl_->sessionId.value()),
            [this](SharedData::OperationsReordered const& evt)
            {
                onOperationsReordered(evt);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onSyncScanPhaseDone", impl_->sessionId.value()),
            [this](std::string const& sessionIdString, bool isLocal)
            {
                const auto routingIt = impl_->syncSessionRouting.find(sessionIdString);
                if (routingIt == impl_->syncSessionRouting.end())
                    return;
                // The per-operation progress entry for this side is no longer interesting.
                const auto& scanId = routingIt->second.scanIds[isLocal ? 1 : 0];
                impl_->syncScanProgressCallbacks.erase(scanId.value());
                if (routingIt->second.onScanPhaseDone)
                    routingIt->second.onScanPhaseDone(isLocal);
            }
        )
    );

    impl_->onUpdate.push_back(
        Nui::RpcClient::autoRegisterFunction(
            fmt::format("OperationQueue::{}::onSyncDiffProgress", impl_->sessionId.value()),
            [this](std::string const& sessionIdString, std::string const& comparedString)
            {
                const auto routingIt = impl_->syncSessionRouting.find(sessionIdString);
                if (routingIt == impl_->syncSessionRouting.end())
                    return;
                if (!routingIt->second.onDiffProgress)
                    return;
                std::uint64_t compared = 0;
                try
                {
                    compared = std::stoull(comparedString);
                }
                catch (std::exception const&)
                {
                    return;
                }
                routingIt->second.onDiffProgress(compared);
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
        else if (
            added.type == SharedData::OperationType::Delete ||
            added.type == SharedData::OperationType::BulkDelete
        )
        {
            using namespace std::string_literals;
            // BulkDelete reuses the DisplayedDeleteOperation card — its body
            // already renders BulkDeleteProgress (current file + filesDeleted
            // / totalFiles), which is exactly what the backend's bulk path
            // emits.  No new card class needed.  When remotePath is absent
            // (the bulk-files aggregate has no single root) we fall back to
            // an empty placeholder; the running progress text takes over
            // before the user sees anything else.
            Log::info(
                "Creating {} operation card for operation id: {}. path: {}",
                added.type == SharedData::OperationType::BulkDelete ? "bulk delete" : "delete",
                added.operationId.value(),
                added.remotePath ? added.remotePath->generic_string() : "???"s
            );
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
        else if (added.type == SharedData::OperationType::ArchiveDownload)
        {
            if (!added.localPath)
            {
                Log::error(
                    "Received OperationAdded for ArchiveDownload id: {} without localPath (archive destination)",
                    added.operationId.value()
                );
                return {};
            }
            return std::make_unique<DisplayedTransferOperation>(
                added.operationId,
                SharedData::OperationType::ArchiveDownload,
                *impl_->confirmDialog,
                added.totalBytes ? static_cast<long long>(*added.totalBytes) : 0,
                *added.localPath,
                std::filesystem::path{},
                [this](OperationCard<DisplayedTransferOperation> const& operation)
                {
                    cancelOperation(operation);
                },
                impl_->autoClean,
                localRefresh
            );
        }
        else if (added.type == SharedData::OperationType::ArchiveUpload)
        {
            if (!added.remotePath)
            {
                Log::error(
                    "Received OperationAdded for ArchiveUpload id: {} without remotePath (archive destination)",
                    added.operationId.value()
                );
                return {};
            }
            return std::make_unique<DisplayedTransferOperation>(
                added.operationId,
                SharedData::OperationType::ArchiveUpload,
                *impl_->confirmDialog,
                added.totalBytes ? static_cast<long long>(*added.totalBytes) : 0,
                std::filesystem::path{},
                *added.remotePath,
                [this](OperationCard<DisplayedTransferOperation> const& operation)
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
    // Kick-to-top is meaningful only for the regular queue. Priority cards do
    // not get a handler — even though the button is rendered, CSS hides it
    // when the parent is the priority list (see .opq-priority-list rule).
    if (added.mode != SharedData::OperationMode::PriorityQueued)
    {
        card->setKickToTopHandler(
            [this, opId = added.operationId]() {
                requestMoveOperation(opId, 0);
            }
        );
    }
    Log::info("Inserting operation id: {} into operation queue", added.operationId.value());
    try
    {
        if (added.mode == SharedData::OperationMode::PriorityQueued)
            impl_->priorityOperations.insert(
                added.operationId, DisplayedOperation{added.operationId, added.type, std::move(card)}
            );
        else
        {
            impl_->operations.insert(
                added.operationId, DisplayedOperation{added.operationId, added.type, std::move(card)}
            );
            impl_->enforceLivePageSize();
        }
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
    // Both Delete (per-directory recursive) and BulkDelete (the file/empty-dir
    // aggregate) cards render via DisplayedDeleteOperation and emit onDeleteProgress.
    if (operation->type() != SharedData::OperationType::Delete &&
        operation->type() != SharedData::OperationType::BulkDelete)
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
    {
        const auto cbIt = impl_->transferProgressCallbacks.find(progress.operationId.value());
        if (cbIt != impl_->transferProgressCallbacks.end() && progress.max > progress.min)
            cbIt->second(static_cast<double>(progress.current - progress.min) / (progress.max - progress.min));
    }
}

void OperationQueue::onScanProgress(SharedData::ScanProgress const& progress)
{
    // Route to any registered per-operation callback (e.g. from enqueueSyncScans).
    {
        const auto cbIt = impl_->syncScanProgressCallbacks.find(progress.operationId.value());
        if (cbIt != impl_->syncScanProgressCallbacks.end())
            cbIt->second(progress);
    }

    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
        return; // Scan-only operations (for sync) may not be in the display map — that is fine.

    if (operation->type() == SharedData::OperationType::Scan ||
        operation->type() == SharedData::OperationType::LocalScan)
    {
        auto* renderer = operation->getCardSpecifically<DisplayedScanOperation>();
        if (renderer)
            renderer->setProgress(progress.totalBytes, progress.currentIndex, progress.totalScanned);
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
    {
        const auto cbIt = impl_->bulkProgressCallbacks.find(progress.operationId.value());
        if (cbIt != impl_->bulkProgressCallbacks.end())
            cbIt->second(progress);
    }
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
    {
        const auto cbIt = impl_->transferProgressCallbacks.find(progress.operationId.value());
        if (cbIt != impl_->transferProgressCallbacks.end() && progress.max > progress.min)
            cbIt->second(static_cast<double>(progress.current - progress.min) / (progress.max - progress.min));
    }
}

void OperationQueue::onArchiveDownloadProgress(SharedData::TransferProgress const& progress)
{
    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received archive download progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::ArchiveDownload)
    {
        Log::error(
            "Received archive download progress for operation id: {} which is not an archive download",
            progress.operationId.value()
        );
        return;
    }
    auto* card = operation->getCardSpecifically<DisplayedTransferOperation>();
    if (!card)
    {
        Log::error(
            "Received archive download progress for operation id: {} which has no transfer renderer",
            progress.operationId.value()
        );
        return;
    }
    card->setProgress(progress);
    {
        const auto cbIt = impl_->transferProgressCallbacks.find(progress.operationId.value());
        if (cbIt != impl_->transferProgressCallbacks.end() && progress.max > progress.min)
            cbIt->second(static_cast<double>(progress.current - progress.min) / (progress.max - progress.min));
    }
}

void OperationQueue::onArchiveUploadProgress(SharedData::TransferProgress const& progress)
{
    auto* operation = impl_->findOperation(progress.operationId);
    if (!operation)
    {
        Log::error("Received archive upload progress for unknown operation id: {}", progress.operationId.value());
        return;
    }
    if (operation->type() != SharedData::OperationType::ArchiveUpload)
    {
        Log::error(
            "Received archive upload progress for operation id: {} which is not an archive upload",
            progress.operationId.value()
        );
        return;
    }
    auto* card = operation->getCardSpecifically<DisplayedTransferOperation>();
    if (!card)
    {
        Log::error(
            "Received archive upload progress for operation id: {} which has no transfer renderer",
            progress.operationId.value()
        );
        return;
    }
    card->setProgress(progress);
    {
        const auto cbIt = impl_->transferProgressCallbacks.find(progress.operationId.value());
        if (cbIt != impl_->transferProgressCallbacks.end() && progress.max > progress.min)
            cbIt->second(static_cast<double>(progress.current - progress.min) / (progress.max - progress.min));
    }
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
    {
        const auto cbIt = impl_->bulkProgressCallbacks.find(progress.operationId.value());
        if (cbIt != impl_->bulkProgressCallbacks.end())
            cbIt->second(progress);
    }
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
            {
                operation->state(SharedData::OperationState::PartialSuccess);
                impl_->recordFailed(completed.operationId);
            }
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
            impl_->recordFailed(completed.operationId);
            break;
        }
        default:
            Log::warn(
                "Received operation completed for operation id: {} with unknown reason: {}",
                completed.operationId.value(),
                static_cast<int>(completed.reason)
            );
    }
    impl_->transferProgressCallbacks.erase(completed.operationId.value());
    impl_->bulkProgressCallbacks.erase(completed.operationId.value());
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

void OperationQueue::addTransferProgressCallback(
    Ids::OperationId const& opId,
    std::function<void(double fraction)> callback
)
{
    impl_->transferProgressCallbacks[opId.value()] = std::move(callback);
}

void OperationQueue::addBulkProgressCallback(
    Ids::OperationId const& aggregateOpId,
    std::function<void(SharedData::BulkProgress const&)> callback
)
{
    impl_->bulkProgressCallbacks[aggregateOpId.value()] = std::move(callback);
}

Nui::Observed<bool>& OperationQueue::pausedState()
{
    return impl_->paused;
}

std::vector<ResumableOp> OperationQueue::snapshotInFlight()
{
    std::vector<ResumableOp> out;
    using Map = ObservedRandomAccessMap<Ids::OperationId, DisplayedOperation, std::map>;
    out.reserve(
        impl_->priorityOperations.observedValues().value().size() +
        impl_->operations.observedValues().value().size()
    );
    std::size_t scanned = 0;
    std::size_t skippedCompleted = 0;
    auto harvest = [&out, &scanned, &skippedCompleted](Map& container)
    {
        for (auto const& entry : container.observedValues().value())
        {
            if (!entry)
                continue;
            ++scanned;
            auto descriptor = entry->resumableDescriptor();
            if (descriptor)
                out.push_back(std::move(*descriptor));
            else
                ++skippedCompleted;
        }
    };
    harvest(impl_->priorityOperations);
    harvest(impl_->operations);
    Log::info(
        "OperationQueue::snapshotInFlight: priority={} normal={} scanned={} skippedCompleted={} captured={}",
        impl_->priorityOperations.observedValues().value().size(),
        impl_->operations.observedValues().value().size(),
        scanned,
        skippedCompleted,
        out.size()
    );
    return out;
}

void OperationQueue::unpause()
{
    if (!impl_->paused.value())
        return;
    Nui::RpcClient::callWithBackChannel(
        fmt::format("OperationQueue::{}::pauseUnpause", impl_->sessionId.value()),
        [this](SharedData::ErrorOrSuccess<> const& result)
        {
            if (result)
            {
                impl_->paused = false;
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        },
        false
    );
}

void OperationQueue::openSyncSession(
    Ids::SyncSessionId syncSessionId,
    std::filesystem::path localPath,
    std::filesystem::path remotePath,
    bool respectIgnoreFiles,
    bool recursive,
    bool ignoreHidden,
    std::function<void(SharedData::ScanProgress const&)> onRemoteProgress,
    std::function<void(SharedData::ScanProgress const&)> onLocalProgress,
    std::function<void(bool)> onScanPhaseDone,
    std::function<void(std::uint64_t)> onDiffProgress
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot open sync session");
        return;
    }

    const auto remoteScanId = Ids::generateOperationId();
    const auto localScanId = Ids::generateOperationId();

    // Register per-operation scan-progress + per-session routing before hitting the
    // backend so no events are missed.
    impl_->syncScanProgressCallbacks[remoteScanId.value()] = std::move(onRemoteProgress);
    impl_->syncScanProgressCallbacks[localScanId.value()] = std::move(onLocalProgress);
    impl_->syncSessionRouting[syncSessionId.value()] = Implementation::SyncSessionRouting{
        .scanIds = {remoteScanId, localScanId},
        .onScanPhaseDone = std::move(onScanPhaseDone),
        .onDiffProgress = std::move(onDiffProgress),
    };

    impl_->fileEngine->openSyncSession(
        localPath,
        remotePath,
        syncSessionId,
        remoteScanId,
        localScanId,
        respectIgnoreFiles,
        recursive,
        ignoreHidden,
        [remoteScanId, localScanId](bool success, std::string const& info)
        {
            if (!success)
                Log::error(
                    "Failed to open sync session (remoteId={}, localId={}): {}",
                    remoteScanId.value(),
                    localScanId.value(),
                    info
                );
        }
    );
}

void OperationQueue::clearSyncSessionRouting(Ids::SyncSessionId syncSessionId)
{
    const auto routingIt = impl_->syncSessionRouting.find(syncSessionId.value());
    if (routingIt == impl_->syncSessionRouting.end())
        return;
    // Drop the per-scan-operation progress entries too in case a phase-done never fired.
    for (auto const& scanId : routingIt->second.scanIds)
        impl_->syncScanProgressCallbacks.erase(scanId.value());
    impl_->syncSessionRouting.erase(routingIt);
}

void OperationQueue::recomputeSyncDiff(
    Ids::SyncSessionId syncSessionId,
    SharedData::Sync::DiffOptions options,
    std::function<void(SharedData::Sync::DiffSummary)> onSummary
)
{
    if (!impl_->fileEngine)
        return;
    impl_->fileEngine->recomputeSyncDiff(syncSessionId, std::move(options), std::move(onSummary));
}

void OperationQueue::loadSyncDiffChildren(
    Ids::SyncSessionId syncSessionId,
    SharedData::Sync::DiffSection section,
    std::string const& parentRelKey,
    std::uint64_t generation,
    std::function<void(std::vector<SharedData::Sync::DiffTreeNode>)> onResolved,
    std::function<void(std::string const&)> onRejected
)
{
    if (!impl_->fileEngine)
        return;
    impl_->fileEngine->loadSyncDiffChildren(
        syncSessionId, section, parentRelKey, generation, std::move(onResolved), std::move(onRejected)
    );
}

void OperationQueue::buildSyncEnqueuePlan(
    Ids::SyncSessionId syncSessionId,
    SharedData::Sync::DiffSection section,
    std::vector<std::string> selectedRelKeys,
    std::uint64_t generation,
    std::function<void(std::vector<SharedData::Sync::EnqueuePlanEntry>)> onResolved,
    std::function<void(std::string const&)> onRejected
)
{
    if (!impl_->fileEngine)
        return;
    impl_->fileEngine->buildSyncEnqueuePlan(
        syncSessionId,
        section,
        std::move(selectedRelKeys),
        generation,
        std::move(onResolved),
        std::move(onRejected)
    );
}

void OperationQueue::cancelSyncDiff(Ids::SyncSessionId syncSessionId)
{
    if (!impl_->fileEngine)
        return;
    impl_->fileEngine->cancelSyncDiff(syncSessionId);
}

void OperationQueue::closeSyncSession(Ids::SyncSessionId syncSessionId)
{
    if (!impl_->fileEngine)
        return;
    impl_->fileEngine->closeSyncSession(syncSessionId);
}

void OperationQueue::createRemoteDirectory(
    std::filesystem::path const& path,
    std::function<void(bool, std::string const&)> onComplete
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot create remote directory");
        onComplete(false, "No file engine set");
        return;
    }
    impl_->fileEngine->createDirectory(path, std::move(onComplete));
}

void OperationQueue::createLocalDirectory(
    std::filesystem::path const& path,
    std::function<void(bool, std::string const&)> onComplete
)
{
    Nui::val args = Nui::val::object();
    args.set("path", path.generic_string());
    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::createDirectory",
        [onComplete = std::move(onComplete)](Nui::val val)
        {
            if (!val.hasOwnProperty("success"))
            {
                onComplete(false, "Invalid response from backend");
                return;
            }
            if (!val["success"].as<bool>())
            {
                onComplete(false, val["error"].as<std::string>());
                return;
            }
            onComplete(true, "Success");
        },
        args
    );
}

void OperationQueue::showMinimizedSync(std::function<void()> onRestore)
{
    impl_->minimizedSyncRestore = std::move(onRestore);
    impl_->minimizedSyncVisible = true;
    impl_->minimizedSyncShine = impl_->minimizedSyncShine.value() + 1;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void OperationQueue::hideMinimizedSync()
{
    impl_->minimizedSyncVisible = false;
    impl_->minimizedSyncRestore = {};
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void OperationQueue::onOperationsReordered(SharedData::OperationsReordered const& evt)
{
    if (!evt.applied)
    {
        // Backend refused or no-oped (queue not paused, op completed, priority
        // op, etc.). Frontend never optimistically reorders, so there is
        // nothing to roll back; this branch exists so the frontend has a
        // definitive resolution signal it can hook into later if needed.
        return;
    }
    impl_->operations.move(evt.operationId, static_cast<std::size_t>(evt.newIndex));
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void OperationQueue::requestMoveOperation(Ids::OperationId const& opId, std::size_t newIndex)
{
    if (!impl_->paused.value())
    {
        // Local guard — backend has its own pause check, but failing fast here
        // avoids a wasted round trip and a refused log on the backend side.
        return;
    }
    Nui::RpcClient::callWithBackChannel(
        fmt::format("OperationQueue::{}::moveOperation", impl_->sessionId.value()),
        [opId](SharedData::ErrorOrSuccess<> const& result)
        {
            if (!result)
                Log::error("moveOperation RPC failed for op {}: {}", opId.value(), result.error.value());
        },
        opId,
        static_cast<std::int32_t>(newIndex)
    );
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
    if (doClean)
    {
        // History may still hold not-yet-completed operations that were
        // force-popped because the live page overflowed; only completed cards
        // are eligible for the auto-clean sweep.  The periodic autoCleanTimer
        // below will keep whittling away any further completions.
        std::erase_if(
            impl_->history,
            [&](auto const& entry)
            {
                if (!entry->isCompletedState())
                    return false;
                impl_->historyIndex.erase(entry->key().value());
                return true;
            }
        );
        impl_->recomputePageCount();
    }
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, name = impl_->persistenceSessionName](bool success, Persistence::State const&)
        {
            if (!success)
                return;

            auto iter = impl_->stateHolder->stateCache().sessions.find(name);
            iter->second.queueOptions->autoRemoveCompletedOperations = impl_->autoClean->value();
            iter->second.queueOptions->liveQueuePageSize = impl_->liveQueuePageSize;
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

Nui::ElementRenderer OperationQueue::makeRegularLiveList()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // Resolve the card element (and its op id) for an arbitrary event target
    // by walking up through `closest("[data-op-id]")`.  Mirrors the resolver
    // in flavor_implementation.cpp:79-88.
    auto resolveCard = [](Nui::val target) -> std::pair<Nui::val, std::string> {
        if (target.isNull() || target.isUndefined())
            return {Nui::val::null(), {}};
        auto node = target.call<Nui::val>("closest", std::string{"[data-op-id]"});
        if (node.isNull() || node.isUndefined())
            return {Nui::val::null(), {}};
        auto attr = node.call<Nui::val>("getAttribute", std::string{"data-op-id"});
        if (attr.isNull() || attr.isUndefined())
            return {Nui::val::null(), {}};
        return {std::move(node), attr.as<std::string>()};
    };

    // Look up an op's current index in the live regular queue.  Returns -1
    // if the op completed/was removed between drag start and drop.
    auto findIndex = [this](std::string const& opId) -> int {
        auto const& deque = impl_->operations.observedValues().value();
        for (std::size_t idx = 0; idx < deque.size(); ++idx)
        {
            if (deque[idx]->key().value() == opId)
                return static_cast<int>(idx);
        }
        return -1;
    };

    return div{
        // `opq-reorderable` marks this list as the only surface where drag and
        // kick-to-top are valid.  Failed / Priority / history pages all reuse
        // .opq-regular-list for their grid layout, but those surfaces must NOT
        // show the affordance — the CSS gate uses .opq-reorderable to scope it.
        class_ = "opq-regular-list opq-reorderable",
        // Delegated drag handlers — one set for the entire list, regardless
        // of card count.  Each handler resolves the affected card via
        // resolveCard().  This is the "many items" optimization the user
        // pointed at in icon_flavor.cpp.
        "dragstart"_event = [this, resolveCard](Nui::WebApi::DragEvent event) {
            if (!impl_->paused.value())
            {
                event.val().call<void>("preventDefault");
                return;
            }
            auto [el, opId] = resolveCard(event.val()["target"]);
            if (opId.empty())
            {
                event.val().call<void>("preventDefault");
                return;
            }
            auto dt = event.val()["dataTransfer"];
            if (dt.isNull() || dt.isUndefined())
                return;
            dt.call<void>("setData", std::string{"text/plain"}, opId);
            dt.set("effectAllowed", std::string{"move"});
            // Visual cue on the dragged card itself (separate from drop target).
            el["classList"].call<void>("add", std::string{"opq-dragging"});
        },
        "dragend"_event = [this, resolveCard](Nui::WebApi::DragEvent event) {
            // Always clear visuals — fires whether the drop succeeded or not.
            auto [el, _opId] = resolveCard(event.val()["target"]);
            if (!el.isNull() && !el.isUndefined())
                el["classList"].call<void>("remove", std::string{"opq-dragging"});
            impl_->clearDragHover();
        },
        "dragover"_event = [this, resolveCard](Nui::WebApi::DragEvent event) {
            if (!impl_->paused.value())
                return;
            // Always preventDefault on dragover or the drop event will not fire.
            event.val().call<void>("preventDefault");
            auto [el, _opId] = resolveCard(event.val()["target"]);
            if (el.isNull() || el.isUndefined())
            {
                impl_->clearDragHover();
                return;
            }
            const auto rect = el.call<Nui::val>("getBoundingClientRect");
            const auto top = rect["top"].as<double>();
            const auto height = rect["height"].as<double>();
            const auto y = event.val()["clientY"].as<double>();
            const bool below = (y - top) > (height * 0.5);
            impl_->setDragHover(std::move(el), below);
        },
        "dragleave"_event = [this](Nui::WebApi::DragEvent event) {
            // Only clear when leaving the list container as a whole (not when
            // moving between cards inside it).  Same guard as
            // flavor_implementation.cpp:144-153.
            auto related = event.val()["relatedTarget"];
            auto current = event.val()["currentTarget"];
            if (!related.isNull() && !related.isUndefined() &&
                !current.isNull() && !current.isUndefined() &&
                current.call<bool>("contains", related))
            {
                return;
            }
            impl_->clearDragHover();
        },
        "drop"_event = [this, resolveCard, findIndex](Nui::WebApi::DragEvent event) {
            if (!impl_->paused.value())
                return;
            event.val().call<void>("preventDefault");

            auto dt = event.val()["dataTransfer"];
            std::string draggedId;
            if (!dt.isNull() && !dt.isUndefined())
            {
                draggedId = dt.call<Nui::val>("getData", std::string{"text/plain"}).as<std::string>();
            }

            const bool below = impl_->currentHoverBelow;
            impl_->clearDragHover();

            if (draggedId.empty())
                return;

            auto [_el, targetId] = resolveCard(event.val()["target"]);
            if (targetId.empty() || targetId == draggedId)
                return;

            const int oldIndex = findIndex(draggedId);
            const int targetIndex = findIndex(targetId);
            if (oldIndex < 0 || targetIndex < 0)
                return;

            // Translate (above|below target) into an absolute desired index.
            // Compensates for the deque shrinking by one when oldIndex < targetIndex.
            int newIndex = below ? (targetIndex + 1) : targetIndex;
            if (oldIndex < newIndex)
                newIndex -= 1;

            const int size = static_cast<int>(impl_->operations.observedValues().value().size());
            if (newIndex < 0)
                newIndex = 0;
            if (newIndex >= size)
                newIndex = size - 1;
            if (newIndex == oldIndex)
                return;

            requestMoveOperation(Ids::makeOperationId(draggedId), static_cast<std::size_t>(newIndex));
        },
    }(
        impl_->operations.observedValues().map(
            [](auto, auto const& element) { return (*element)(); }
        )
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
                impl_->priorityOperations.observedValues().value().size() +
                impl_->history.size()
            )
        );
    };

    // clang-format off
    return div{
        class_ = "operation-queue",
        // Single source of truth for "kick-to-top + drag are active".  Optional
        // returning nullopt removes the attribute entirely so CSS rules like
        // `.operation-queue[data-paused] .op-kick-up` only match while paused.
        // Avoids putting an Observed<bool> on every card.
        "data-paused"_attr = observe(impl_->paused).generate([this]() -> std::optional<std::string> {
            if (impl_->paused.value())
                return std::optional<std::string>{"true"};
            return std::nullopt;
        }),
        tabIndex = 0,
        "keydown"_event = [this](Nui::WebApi::KeyboardEvent event) {
            const auto key = event.key();
            if (key != "PageUp" && key != "PageDown")
                return;
            const auto total = impl_->pageCount.value();
            if (total <= 1)
                return;
            event.stopPropagation();
            event.preventDefault();
            const int current = impl_->currentPage.value();
            const int next = key == "PageDown"
                ? std::min(total - 1, current + 1)
                : std::max(0, current - 1);
            if (next == current)
                return;
            impl_->followLive = (next == total - 1);
            impl_->currentPage = next;
            Nui::globalEventContext.executeActiveEventsImmediately();
        },
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
            // Conditional render: avoid creating the button in the DOM until
            // a minimize actually happens.  If we instead toggled visibility
            // via `style=display:none`, the button's shine-color `style` and
            // the visibility `style` would both bind to the element's `style`
            // attribute and the last-wins merge clobbered `display:none`,
            // leaving the button visible on page load.
            fragment(
                observe(impl_->minimizedSyncVisible),
                [this]() -> Nui::ElementRenderer {
                    if (!impl_->minimizedSyncVisible.value())
                        return Nui::nil();
                    namespace Snc = ScriptNuiComponents;
                    return Snc::button({
                        .text = language->getObserved("operationQueue", "resumeMinimizedSync"),
                        .icon = Ui5Icons::synchronize(),
                        .attributes = {
                            style = "align-self: stretch;",
                            Nui::Attributes::title = language->get("operationQueue", "restoreMinimizedSyncTitle"),
                            onClick = [this]() {
                                if (impl_->minimizedSyncRestore)
                                    impl_->minimizedSyncRestore();
                            },
                        },
                        .styleVariant = Snc::StyleVariant::Regular,
                        .shine = Snc::ShineOptions{
                            .trigger = &impl_->minimizedSyncShine,
                            .color = "rgba(80, 220, 120, 0.9)",
                        },
                    });
                }
            ),
            div{
                class_ = "opq-summary"
            }(
                observe(impl_->operations.observedValues(), impl_->priorityOperations.observedValues())
                    .generate(makeSummaryText)
            )
        ),
        // Tab bar — Queue (live + history via pagination) vs Failed vs Priority.
        // Priority lives in its own tab (rather than on top of the live page)
        // because it's intentionally non-reorderable — keeping it on the same
        // surface as the drag-reorderable regular queue would be confusing UX.
        div{class_ = "opq-tabs"}(
            div{
                class_ = observe(impl_->activeTab).generate([this]() {
                    return std::string{"opq-tab"} + (impl_->activeTab.value() == 0 ? " selected" : "");
                }),
                onClick = [this](Nui::val) { impl_->activeTab = 0; },
            }(span{}(language->getObserved("operationQueue", "tabQueue"))),
            div{
                class_ = observe(impl_->activeTab).generate([this]() {
                    return std::string{"opq-tab"} + (impl_->activeTab.value() == 2 ? " selected" : "");
                }),
                onClick = [this](Nui::val) { impl_->activeTab = 2; },
            }(span{}(language->getObserved("operationQueue", "tabPriority")))
            ,
            div{
                class_ = observe(impl_->activeTab).generate([this]() {
                    return std::string{"opq-tab"} + (impl_->activeTab.value() == 1 ? " selected" : "");
                }),
                onClick = [this](Nui::val) { impl_->activeTab = 1; },
            }(
                span{}(language->getObserved("operationQueue", "tabFailed")),
                span{class_ = "opq-tab-count"}(
                    observe(impl_->failed).generate([this]() {
                        return fmt::format("{}", impl_->failed.value().size());
                    })
                )
            )
        ),
        // Pagination bar — hidden when pageCount == 1 or when Failed tab is active.
        div{
            class_ = observe(impl_->activeTab, impl_->pageCount).generate([this]() {
                const bool hidden = impl_->activeTab.value() != 0 || impl_->pageCount.value() <= 1;
                return std::string{"opq-pagination-host"} + (hidden ? " hidden" : "");
            }),
        }(
            Snc::pagination({
                .pageCount = &impl_->pageCount,
                .currentPage = &impl_->currentPage,
                .onPageChange = [this](int newPage) {
                    // Auto-follow live only while the user is viewing the live
                    // page; clicking any history page opts them out until they
                    // return to the live page.
                    impl_->followLive = (newPage == impl_->pageCount.value() - 1);
                    impl_->currentPage = newPage;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
            })
        ),
        // Main content — switches between live regular-queue page, a static
        // history-page slice, the Failed tab, or the new Priority tab.
        div{
            class_ = "opq-list"
        }(
            // Only subscribe to tab + paging state here.  `failed` and
            // `priorityOperations` mutations must NOT tear down the live
            // regular-queue ranges below, or every new addition would wipe
            // the live list and re-create all card nodes.
            Nui::observe(impl_->activeTab, impl_->currentPage, impl_->pageCount),
            [this]() -> Nui::ElementRenderer
            {
                if (impl_->activeTab.value() == 1)
                {
                    // Failed tab: render the current snapshot.  We pass the
                    // Observed deque into Nui::range so Failed-list updates
                    // diff incrementally while the user is viewing this tab.
                    return div{class_ = "opq-regular-list"}(
                        Nui::range(impl_->failed),
                        [](long long, auto const& entry) -> Nui::ElementRenderer {
                            return (*entry)();
                        }
                    );
                }

                if (impl_->activeTab.value() == 2)
                {
                    // Priority tab: non-reorderable.  Cards still render with
                    // the kick-to-top button DOM, but no handler is installed
                    // (see onOperationAdded) and CSS hides the button on this
                    // surface via .opq-priority-list .op-kick-up { display:none }.
                    return div{class_ = "opq-priority-list"}(
                        Nui::range(impl_->priorityOperations.observedValues()),
                        [](long long, auto const& element) -> Nui::ElementRenderer {
                            return (*element)();
                        }
                    );
                }

                const int page = impl_->currentPage.value();
                const int pageMax = impl_->pageCount.value() - 1;

                if (page >= pageMax)
                {
                    // Live regular-queue page.  Drag handlers are delegated at
                    // this list level (not per-card) — see icon_flavor.cpp /
                    // flavor_implementation.cpp for the same pattern in the
                    // file explorer.  This keeps DnB cost O(1) regardless of
                    // queue size.
                    return makeRegularLiveList();
                }

                // History page: static slice of evicted cards.  Not reorderable;
                // the .opq-regular-list class still applies but no draggable
                // attribute is set on history cards' wrapping (the cards
                // themselves carry draggable=true but the parent has no drag
                // handler so it fizzles).
                const int pageSize = std::max(1, impl_->liveQueuePageSize);
                const int begin = page * pageSize;
                const int end = std::min(begin + pageSize, static_cast<int>(impl_->history.size()));
                std::vector<Nui::ElementRenderer> cards;
                cards.reserve(static_cast<std::size_t>(std::max(0, end - begin)));
                for (int idx = begin; idx < end; ++idx)
                    cards.push_back((*impl_->history[idx])());
                return div{class_ = "opq-regular-list"}(
                    Nui::range(std::move(cards)),
                    [](long long, auto const& card) -> Nui::ElementRenderer { return card; }
                );
            }
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
    bool createMissingDirectories,
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
    impl_->fileEngine->addDownload(
        remoteItem, localItem, std::move(onComplete), allowOverwrite, insertRefresh, createMissingDirectories, mode
    );
}
void OperationQueue::enqueueUpload(
    NuiFileExplorer::Item const& remoteItem,
    NuiFileExplorer::Item const& localItem,
    std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
    bool allowOverwrite,
    bool insertRefresh,
    bool createMissingDirectories,
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
    impl_->fileEngine->addUpload(
        remoteItem, localItem, std::move(onComplete), allowOverwrite, insertRefresh, createMissingDirectories, mode
    );
}
void OperationQueue::enqueueArchiveDownload(
    std::vector<SharedData::DirectoryEntry> entries,
    std::filesystem::path const& localArchivePath,
    int compressionCodec,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode,
    std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onOperationCreated
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue archive download");
        onOperationCreated(std::nullopt, "No file engine set");
        return;
    }

    Log::info(
        "Frontend Operation Queue archive download: {} entries → {} (codec={}, level={})",
        entries.size(),
        localArchivePath.generic_string(),
        compressionCodec,
        compressionLevel
    );
    impl_->fileEngine->addArchiveDownload(
        std::move(entries),
        localArchivePath,
        compressionCodec,
        compressionLevel,
        mayOverwrite,
        mode,
        [onOperationCreated = std::move(onOperationCreated)](
            std::optional<Ids::OperationId> opId, std::string const& info
        )
        {
            onOperationCreated(opId, info);
        }
    );
}

void OperationQueue::enqueueArchiveUpload(
    std::vector<std::filesystem::path> localPaths,
    std::filesystem::path const& remoteArchivePath,
    int compressionCodec,
    int compressionLevel,
    bool mayOverwrite,
    SharedData::OperationMode mode,
    std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onOperationCreated
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue archive upload");
        onOperationCreated(std::nullopt, "No file engine set");
        return;
    }

    Log::info(
        "Frontend Operation Queue archive upload: {} paths → {} (codec={}, level={})",
        localPaths.size(),
        remoteArchivePath.generic_string(),
        compressionCodec,
        compressionLevel
    );
    impl_->fileEngine->addArchiveUpload(
        std::move(localPaths),
        remoteArchivePath,
        compressionCodec,
        compressionLevel,
        mayOverwrite,
        mode,
        [onOperationCreated = std::move(onOperationCreated)](
            std::optional<Ids::OperationId> opId, std::string const& info
        )
        {
            onOperationCreated(opId, info);
        }
    );
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

void OperationQueue::enqueueBulkDownload(
    std::vector<SharedData::BulkAddEntry> entries,
    bool allowOverwrite,
    bool insertRefresh,
    SharedData::OperationMode mode,
    std::function<void(Ids::OperationId const& opId, bool success)> onEachComplete,
    std::function<void(bool success, std::string const& info)> onBulkAck,
    std::function<void(std::vector<Ids::OperationId> const&)> onEnqueued
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue bulk download");
        if (onBulkAck)
            onBulkAck(false, "No file engine set");
        return;
    }

    // Pre-allocate the OperationIds so per-entry completion callbacks can be
    // registered BEFORE the RPC returns.  Otherwise progress events for the
    // first entries might arrive and be dropped before their callbacks exist.
    // We reserve one extra id at the back for the aggregate bulk-download card
    // so it cannot collide with any per-entry id.
    std::vector<Ids::OperationId> operationIds;
    operationIds.reserve(entries.size() + 1);
    for (std::size_t idx = 0; idx < entries.size(); ++idx)
        operationIds.push_back(Ids::generateOperationId());
    operationIds.push_back(Ids::generateOperationId()); // aggregate bulk-card id

    if (onEachComplete)
    {
        // Register completion callbacks only for the per-entry ids — the
        // aggregate card's completion surfaces through the operation-queue
        // observer, not this per-entry callback.
        for (std::size_t idx = 0; idx < entries.size(); ++idx)
        {
            auto const& opId = operationIds[idx];
            impl_->completionCallbacks.emplace(
                opId.value(),
                [onEachComplete, opId](bool success) { onEachComplete(opId, success); }
            );
        }
    }

    if (onEnqueued)
        onEnqueued(operationIds);

    Log::info(
        "Frontend Operation Queue bulk download: {} entries, one RPC",
        entries.size()
    );
    impl_->fileEngine->addBulkDownload(
        std::move(entries),
        std::move(operationIds),
        allowOverwrite,
        insertRefresh,
        mode,
        std::move(onBulkAck)
    );
}

void OperationQueue::enqueueBulkUpload(
    std::vector<SharedData::BulkAddEntry> entries,
    bool allowOverwrite,
    bool insertRefresh,
    SharedData::OperationMode mode,
    std::function<void(Ids::OperationId const& opId, bool success)> onEachComplete,
    std::function<void(bool success, std::string const& info)> onBulkAck,
    std::function<void(std::vector<Ids::OperationId> const&)> onEnqueued
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue bulk upload");
        if (onBulkAck)
            onBulkAck(false, "No file engine set");
        return;
    }

    // Reserve one extra id at the back for the aggregate bulk-upload card —
    // see enqueueBulkDownload for rationale.
    std::vector<Ids::OperationId> operationIds;
    operationIds.reserve(entries.size() + 1);
    for (std::size_t idx = 0; idx < entries.size(); ++idx)
        operationIds.push_back(Ids::generateOperationId());
    operationIds.push_back(Ids::generateOperationId()); // aggregate bulk-card id

    if (onEachComplete)
    {
        for (std::size_t idx = 0; idx < entries.size(); ++idx)
        {
            auto const& opId = operationIds[idx];
            impl_->completionCallbacks.emplace(
                opId.value(),
                [onEachComplete, opId](bool success) { onEachComplete(opId, success); }
            );
        }
    }

    if (onEnqueued)
        onEnqueued(operationIds);

    Log::info("Frontend Operation Queue bulk upload: {} entries, one RPC", entries.size());
    impl_->fileEngine->addBulkUpload(
        std::move(entries),
        std::move(operationIds),
        allowOverwrite,
        insertRefresh,
        mode,
        std::move(onBulkAck)
    );
}

void OperationQueue::enqueueResumable(ResumableOp const& op)
{
    if (!impl_->fileEngine)
    {
        Log::error(
            "OperationQueue::enqueueResumable: no file engine, cannot resume kind={} src={}",
            static_cast<int>(op.kind),
            op.src.generic_string()
        );
        return;
    }

    Log::info(
        "OperationQueue::enqueueResumable: dispatching kind={} src={} dst={}",
        static_cast<int>(op.kind),
        op.src.generic_string(),
        op.dst.generic_string()
    );

    auto reportSingle = [kind = op.kind, src = op.src, dst = op.dst](
                            std::optional<Ids::OperationId> const& opId, std::string const& info)
    {
        if (opId)
        {
            Log::info(
                "Resumed operation kind={} src={} dst={} as id {}",
                static_cast<int>(kind),
                src.generic_string(),
                dst.generic_string(),
                opId->value()
            );
        }
        else
        {
            Log::error(
                "Failed to re-enqueue operation kind={} src={} dst={}: {}",
                static_cast<int>(kind),
                src.generic_string(),
                dst.generic_string(),
                info
            );
        }
    };

    // Build a minimal Item from a path.  The size hint biases the backend
    // toward the streaming/big-file code path so a partial transfer can
    // resume; the small-file path doesn't honour tryContinue.
    auto makeItem = [](std::filesystem::path const& path) {
        SharedData::DirectoryEntry entry{};
        entry.path = path;
        entry.type = SharedData::FileType::Regular;
        entry.size = static_cast<std::uint64_t>(Constants::bigFileCutOff) + 1;
        return NuiFileExplorer::Item{entry};
    };

    switch (op.kind)
    {
        case ResumableOp::Kind::Download:
            enqueueDownload(
                makeItem(op.src),
                makeItem(op.dst),
                reportSingle,
                op.allowOverwrite,
                /*insertRefresh=*/true,
                op.createMissingDirectories,
                SharedData::OperationMode::Queued
            );
            break;
        case ResumableOp::Kind::Upload:
            enqueueUpload(
                makeItem(op.dst),
                makeItem(op.src),
                reportSingle,
                op.allowOverwrite,
                /*insertRefresh=*/true,
                op.createMissingDirectories,
                SharedData::OperationMode::Queued
            );
            break;
        case ResumableOp::Kind::Rename:
            enqueueRename(op.src, op.dst, reportSingle, SharedData::OperationMode::Queued);
            break;
        case ResumableOp::Kind::Delete:
            enqueueDelete(
                {op.src},
                op.recursive,
                [src = op.src](std::optional<std::vector<Ids::OperationId>> const& ids, std::string const& info) {
                    if (ids)
                        Log::info("Resumed delete of {} ({} op(s) created)", src.generic_string(), ids->size());
                    else
                        Log::error("Failed to re-enqueue delete of {}: {}", src.generic_string(), info);
                },
                SharedData::OperationMode::Queued
            );
            break;
        case ResumableOp::Kind::BulkDownload:
        case ResumableOp::Kind::BulkUpload:
        case ResumableOp::Kind::BulkDelete:
            // Bulk kinds are adopted via SessionManager::adoptBulkResume —
            // see Session::applySnapshot.  This helper handles only the
            // single-file kinds.
            break;
    }
}

void OperationQueue::enqueueBulkDelete(
    std::vector<SharedData::BulkAddEntry> entries,
    bool insertRefresh,
    SharedData::OperationMode mode,
    std::function<void(bool success)> onBulkComplete,
    std::function<void(bool success, std::string const& info)> onBulkAck,
    std::function<void(Ids::OperationId const&)> onEnqueued
)
{
    if (!impl_->fileEngine)
    {
        Log::error("No file engine set for operation queue, cannot enqueue bulk delete");
        if (onBulkAck)
            onBulkAck(false, "No file engine set");
        return;
    }

    // Single OperationId for the aggregate file-bulk card so the per-bulk
    // completion callback can be registered before the RPC returns.  Per-
    // directory cards (created backend-side for directory entries) use
    // their own ids and aren't tracked here — those are normal Delete
    // operations the user can monitor in the queue.
    const auto bulkOperationId = Ids::generateOperationId();
    if (onBulkComplete)
    {
        impl_->completionCallbacks.emplace(
            bulkOperationId.value(),
            [onBulkComplete](bool success) { onBulkComplete(success); }
        );
    }

    if (onEnqueued)
        onEnqueued(bulkOperationId);

    Log::info("Frontend Operation Queue bulk delete: {} entries, one RPC", entries.size());
    impl_->fileEngine->addBulkDelete(
        std::move(entries),
        bulkOperationId,
        insertRefresh,
        mode,
        std::move(onBulkAck)
    );
}