#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

#include <shared_data/file_operations/bulk_delete_progress.hpp>

struct DisplayedDeleteOperation : public OperationCard<DisplayedDeleteOperation>
{
  public:
    DisplayedDeleteOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        SharedData::OperationType type,
        std::filesystem::path removePath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction
    )
        : OperationCard{
              type,
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

        // clang-format off
        return div{
            class_ = "opq-body opq-single"
        }(
            div{}(
                div{
                    class_ = "opq-transfer-route",
                    alt = removePath_.generic_string()
                }(
                    span{
                        class_ = "opq-route-segment"
                    }(
                        observe(currentFile),
                        [this](){
                            if (type_ == SharedData::OperationType::BulkDelete)
                                return fmt::format("Deleting selected items in {}", removePath_.generic_string());
                            if (currentFile.value().empty())
                                return fmt::format("Deleting: '{}'", removePath_.generic_string());
                            return fmt::format("Deleting: '{}'", currentFile.value());
                        }
                    )
                ),
                span{
                    class_ = "opq-status-text"
                }(
                    observe(filesDeleted_, totalFiles_),
                    [this](){
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
    std::filesystem::path removePath_;
    Components::ProgressBar progressBar_;
};