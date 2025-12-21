#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/terminal/file_engine.hpp>

#include <shared_data/file_operations/download_progress.hpp>
#include <shared_data/file_operations/bulk_download_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/is_paused.hpp>
#include <shared_data/error_or_success.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <chrono>

class OperationQueue
{
  public:
    constexpr static std::chrono::seconds autoRemoveTime{5};

    OperationQueue(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::string persistenceSessionName,
        ConfirmDialog* confirmDialog);

    ROAR_PIMPL_SPECIAL_FUNCTIONS(OperationQueue);

    void activate(FileEngine* fileEngine, Ids::SessionId sessionId);

    void enqueueDownload(
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        std::function<void(std::optional<Ids::OperationId> const&)> onComplete);
    void enqueueUpload(
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        std::function<void(std::optional<Ids::OperationId> const&)> onComplete);
    void enqueueRename(
        std::filesystem::path const& oldPath,
        std::filesystem::path const& newPath,
        std::function<void(std::optional<Ids::OperationId> const&)> onComplete);
    void enqueueDelete(
        std::filesystem::path const& path,
        std::function<void(std::optional<Ids::OperationId> const&)> onComplete);

    Nui::ElementRenderer operator()();

  private:
    template <typename OperationCard>
    void cancelOperation(OperationCard const& operation);

    void onOperationAdded(SharedData::OperationAdded const& added);
    void onDownloadProgress(SharedData::DownloadProgress const& progress);
    void onBulkDownloadProgress(SharedData::BulkDownloadProgress const& progress);
    void onScanProgress(SharedData::ScanProgress const& progress);
    void onOperationCompleted(Nui::val val);
    void onIsPaused(SharedData::ErrorOrSuccess<SharedData::IsPaused> const& result);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};