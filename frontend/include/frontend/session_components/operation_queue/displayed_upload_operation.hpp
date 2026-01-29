#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

#include <shared_data/file_operations/upload_progress.hpp>

class DisplayedUploadOperation : public OperationCard<DisplayedUploadOperation>
{
  public:
    DisplayedUploadOperation(
        Ids::OperationId operationId,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown
    )
        : OperationCard{
              SharedData::OperationType::Upload,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown)
          }
        , progressBar_{{
              .height = std::string{progressHeight},
              .min = 0,
              .max = 0,
              .showMinMax = true,
              .byteMode = true,
          }}
        , localPath_{std::move(localPath)}
        , remotePath_{std::move(remotePath)}
    {}

    Nui::ElementRenderer body() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        // clang-format off
            return div{
                bodyClass()
            }(
                progressBar_()
            );
        // clang-format on
    }

    void setProgress(SharedData::UploadProgress progress)
    {
        progressBar_.max(progress.max);
        progressBar_.setProgress(progress.current - progress.min);
    }

    std::string title() const override
    {
        return fmt::format("Upload '{}' to '{}'", localPath_.generic_string(), remotePath_.generic_string());
    }

    bool warrantsCancelConfirm() const override
    {
        return !isCompletedState() && progressBar_.max() > 10'000'000; // 10 MB
    }

  private:
    Components::ProgressBar progressBar_;
    std::filesystem::path localPath_;
    std::filesystem::path remotePath_;
};