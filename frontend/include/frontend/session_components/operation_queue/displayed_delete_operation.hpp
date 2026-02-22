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
              .max = 1,
              .showMinMax = true,
              .byteMode = false,
          })
    {}

    bool warrantsCancelConfirm() const override
    {
        return false;
    }

    void setProgress(SharedData::BulkDeleteProgress const& progress)
    {
        if (currentFile.value() != progress.currentFile)
            currentFile = progress.currentFile;

        progressBar_.setProgress(progress.filesDeleted);
        progressBar_.max(static_cast<long long>(progress.totalFiles));

        Nui::globalEventContext.executeActiveEventsImmediately();
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
                        return fmt::format("Deleting: '{}'", currentFile.value());
                    }
                ),
                progressBar_(),
                div{}()
            );
        // clang-format on
    }

  private:
    Nui::Observed<std::string> currentFile{""};
    std::filesystem::path removePath_;
    Components::ProgressBar progressBar_;
};