#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

#include <shared_data/file_operations/bulk_progress.hpp>
#include <utility/format_bytes.hpp>

struct DisplayedBulkOperation : public OperationCard<DisplayedBulkOperation>
{
  public:
    DisplayedBulkOperation(
        Ids::OperationId operationId,
        SharedData::OperationType type,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown
    )
        : OperationCard{type, std::move(operationId), std::move(doRemoveSelf), std::move(doDeletionCountdown)}
        , localPath_{std::move(localPath)}
        , remotePath_{std::move(remotePath)}
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

    std::string statusText() const
    {
        return fmt::format(
            "{}/{} - {}/s",
            fileCurrentIndex.value(),
            fileCount.value(),
            Utility::formatBytes(bytesPerSecond.value(), Utility::determineOrderOfMagnitude(bytesPerSecond.value()))
        );
    }

    void setProgress(SharedData::BulkProgress const& progress)
    {
        if (currentFile.value() != progress.currentFile)
            currentFile = progress.currentFile;

        fileProgressBar_.setProgress(progress.currentFileBytes);
        fileProgressBar_.max(static_cast<long long>(progress.currentFileTotalBytes));

        totalProgressBar_.setProgress(progress.bytesCurrent);
        totalProgressBar_.max(static_cast<long long>(progress.bytesTotal));

        bytesPerSecond = progress.bytesPerSecond;

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

        // clang-format off
        return div{
            class_ = "opq-body opq-bulk"
        }(
            div{}(
                span{
                    style = "flex-grow: 1"
                }(
                    observe(currentFile),
                    [this](){
                        if (currentFile.empty())
                            return fmt::format("{} {} {}", localPath_.generic_string(), type_ == SharedData::OperationType::BulkUpload ? "->" : "<-", remotePath_.generic_string());
                        return fmt::format("{}:", currentFile.value());
                    }
                ),
                span{}(
                    observe(fileCurrentIndex, fileCount, bytesPerSecond),
                    [this](){
                        return statusText();
                    }
                )
            ),
            div{}(
                fileProgressBar_(),
                totalProgressBar_()
            )
        );
        // clang-format on
    }

  private:
    std::filesystem::path localPath_;
    std::filesystem::path remotePath_;
    Nui::Observed<std::string> currentFile{""};
    Nui::Observed<std::uint64_t> fileCurrentIndex{0ull};
    Nui::Observed<std::uint64_t> fileCount{0ull};
    Nui::Observed<std::make_signed_t<std::size_t>> bytesPerSecond{0};

    Components::ProgressBar fileProgressBar_;
    Components::ProgressBar totalProgressBar_;
};