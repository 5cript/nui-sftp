#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>
#include <frontend/session_components/operation_queue/scan_phase_status.hpp>

#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <utility/language.hpp>

struct DisplayedDeleteOperation
    : public OperationCard<DisplayedDeleteOperation>
    , public ScanningCard
{
  public:
    DisplayedDeleteOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        SharedData::OperationType type,
        std::string displayPath,
        Paths::RemotePath target,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction,
        bool scanning = false
    )
        : OperationCard{
              type,
              confirmDialog,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown),
              std::move(onCompleteAction)
          }
        , ScanningCard{scanning}
        , displayPath_{std::move(displayPath)}
        , target_{std::move(target)}
        , recursive_{scanning}
        , progressBar_({
              .height = std::string{progressHeight},
              .min = 0,
              .max = 0,
              .showMinMax = true,
              .byteMode = false,
          })
    {}

    bool warrantsCancelConfirm() const override
    {
        return false;
    }

    void state(SharedData::OperationState newState) override
    {
        OperationCard::state(newState);
        if (isCompletedState())
            progressBar_.setZeroAsComplete();
    }

    void setProgress(SharedData::BulkDeleteProgress const& progress)
    {
        scanPhase_.end();
        if (currentFile.value() != progress.currentFile)
            currentFile = progress.currentFile;

        if (filesDeleted_.value() != progress.filesDeleted)
            filesDeleted_ = progress.filesDeleted;
        if (totalFiles_.value() != progress.totalFiles)
            totalFiles_ = progress.totalFiles;

        progressBar_.setProgress(progress.filesDeleted);
        progressBar_.max(static_cast<long long>(progress.totalFiles));

        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    std::optional<ResumableOp> resumableDescriptor() const override
    {
        // A local delete runs in the local operation queue, which a reconnect does not touch.
        if (isCompletedState() || type_ == SharedData::OperationType::LocalDelete)
            return std::nullopt;
        // A bulk delete is re-issued by the backend from its own backup.
        if (type_ == SharedData::OperationType::BulkDelete)
            return Resumable::Bulk{.operationId = operationId()};
        if (target_.empty())
            return std::nullopt;
        return Resumable::Delete{.target = target_, .recursive = recursive_};
    }

    Nui::ElementRenderer body() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
        return div{
            class_ = "opq-body opq-single"
        }(
            div{}(
                div{
                    class_ = "opq-transfer-route",
                    alt = displayPath_
                }(
                    span{
                        class_ = "opq-route-segment"
                    }(
                        observe(currentFile),
                        [this](){
                            if (type_ == SharedData::OperationType::BulkDelete)
                                return fmt::format(fmt::runtime(language->get("operationQueue", "deletingSelectedIn")), displayPath_);
                            if (type_ == SharedData::OperationType::LocalDelete)
                                return fmt::format(fmt::runtime(language->get("operationQueue", "deletingLocallyIn")), displayPath_);
                            if (currentFile.value().empty())
                                return fmt::format(fmt::runtime(language->get("operationQueue", "deletingItem")), displayPath_);
                            return fmt::format(fmt::runtime(language->get("operationQueue", "deletingItem")), currentFile.value());
                        }
                    )
                ),
                span{
                    class_ = "opq-status-text"
                }(
                    observe(filesDeleted_, totalFiles_, state_, scanPhase_.scanning, scanPhase_.totalScanned),
                    [this](){
                        if (scanPhase_.scanning.value() && !isCompletedState())
                            return scanPhase_.text();
                        return fmt::format("{}/{}", filesDeleted_.value(), totalFiles_.value());
                    }
                )
            ),
            progressBar_()
        );
        // clang-format on
    }

  private:
    Nui::Observed<std::string> currentFile{""};
    Nui::Observed<std::uint64_t> filesDeleted_{0ull};
    Nui::Observed<std::uint64_t> totalFiles_{0ull};
    std::string displayPath_;
    /** @brief The remote item a single delete removes, unset for local and bulk deletes. */
    Paths::RemotePath target_;
    /** @brief The delete scanned a directory, so it removes content and resumes doing the same. */
    bool recursive_{false};
    Components::ProgressBar progressBar_;
};