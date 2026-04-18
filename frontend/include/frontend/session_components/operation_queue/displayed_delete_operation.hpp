#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

#include <shared_data/file_operations/bulk_delete_progress.hpp>

struct DisplayedDeleteOperation : public OperationCard<DisplayedDeleteOperation>
{
  public:
    DisplayedDeleteOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        std::filesystem::path removePath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction
    )
        : OperationCard{
              SharedData::OperationType::Delete,
              confirmDialog,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown),
              std::move(onCompleteAction)
          }
        , removePath_{std::move(removePath)}
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
        if (currentFile.value() != progress.currentFile)
            currentFile = progress.currentFile;

        progressBar_.setProgress(progress.filesDeleted);
        progressBar_.max(static_cast<long long>(progress.totalFiles));

        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    std::optional<ResumableOp> resumableDescriptor() const override
    {
        if (isCompletedState())
            return std::nullopt;
        ResumableOp out;
        // BulkDelete reuses this card class; differentiate so the resume
        // routes to the backend's saved-entry-list path instead of the
        // single-Delete frontend path.
        if (type_ == SharedData::OperationType::BulkDelete)
        {
            out.kind = ResumableOp::Kind::BulkDelete;
            out.operationId = operationId();
        }
        else
        {
            out.kind = ResumableOp::Kind::Delete;
            out.recursive = true;
        }
        out.src = removePath_;
        return out;
    }

    Nui::ElementRenderer body() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        Log::info("Rendering bulk delete operation body");

        // clang-format off
            return fragment(
                span{}(
                    observe(currentFile),
                    [this](){
                        return fmt::format("Deleting: '{}'", currentFile.value().empty() ? removePath_.generic_string() : currentFile.value());
                    }
                ),
                progressBar_("grid-column: 3 / 5;")
            );
        // clang-format on
    }

  private:
    Nui::Observed<std::string> currentFile{""};
    std::filesystem::path removePath_;
    Components::ProgressBar progressBar_;
};