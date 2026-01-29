#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

#include <shared_data/file_operations/bulk_upload_progress.hpp>

struct DisplayedBulkUploadOperation : public OperationCard<DisplayedBulkUploadOperation>
{
  public:
    DisplayedBulkUploadOperation(
        Ids::OperationId operationId,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown
    )
        : OperationCard{
              SharedData::OperationType::BulkUpload,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown)
          }
        , fileProgressBar_({
              .height = std::string{progressHeight},
              .min = 0,
              .max = 1,
              .showMinMax = true,
              .byteMode = true,
          })
        , totalProgressBar_({
              .height = std::string{progressHeight},
              .min = 0,
              .max = 1,
              .showMinMax = true,
              .byteMode = true,
          })
    {}

    bool warrantsCancelConfirm() const override
    {
        return true;
    }

    std::string statusText() const override
    {
        return fmt::format("Total Progress - File {}/{}", fileCurrentIndex.value(), fileCount.value());
    }

    std::string title() const override
    {
        return "Bulk Upload";
    }

    void setProgress(SharedData::BulkUploadProgress const& progress)
    {
        if (currentFile.value() != progress.currentFile)
            currentFile = progress.currentFile;

        fileProgressBar_.setProgress(progress.currentFileBytes);
        fileProgressBar_.max(static_cast<long long>(progress.currentFileTotalBytes));

        totalProgressBar_.setProgress(progress.bytesCurrent);
        totalProgressBar_.max(static_cast<long long>(progress.bytesTotal));

        fileCurrentIndex = progress.fileCurrentIndex;
        fileCount = progress.fileCount;

        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    Nui::ElementRenderer body() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        Log::info("Rendering bulk download operation body");

        // clang-format off
            return div{
                bodyClass()
            }(
                div {
                    style = "margin-top: 8px; font-size: 13px; color: var(--muted);"
                }(
                    span{}(
                        observe(currentFile),
                        [this](){
                            return fmt::format("Current File: '{}'", currentFile.value());
                        }
                    ),
                    fileProgressBar_(),
                    span{}(
                        observe(fileCurrentIndex, fileCount),
                        [this](){
                            return statusText();
                        }
                    ),
                    totalProgressBar_()
                )
            );
        // clang-format on
    }

  private:
    Nui::Observed<std::string> currentFile{""};
    Nui::Observed<std::uint64_t> fileCurrentIndex{0ull};
    Nui::Observed<std::uint64_t> fileCount{0ull};

    Components::ProgressBar fileProgressBar_;
    Components::ProgressBar totalProgressBar_;
};