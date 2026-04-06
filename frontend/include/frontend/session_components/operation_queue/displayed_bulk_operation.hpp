#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>
#include <frontend/components/svg/arrow_right.hpp>

#include <shared_data/file_operations/bulk_progress.hpp>
#include <utility/format_bytes.hpp>

struct DisplayedBulkOperation : public OperationCard<DisplayedBulkOperation>
{
  public:
    DisplayedBulkOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        SharedData::OperationType type,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
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
                div{
                    style = "flex-grow: 1; overflow: hidden; min-width: 0;"
                }(
                    observe(currentFile),
                    [this]() -> Nui::ElementRenderer {
                        const auto& srcPath = type_ == SharedData::OperationType::BulkUpload ? localPath_ : remotePath_;
                        const auto& dstPath = type_ == SharedData::OperationType::BulkUpload ? remotePath_ : localPath_;
                        if (currentFile.value().empty())
                        {
                            return div{
                                class_ = "opq-transfer-route",
                                alt = fmt::format("{} \u2192 {}", srcPath.generic_string(), dstPath.generic_string())
                            }(
                                span{
                                    class_ = "opq-route-segment"
                                }(srcPath.generic_string()),
                                span{
                                    class_ = "opq-route-arrow"
                                }(Svgs::arrowRight()),
                                span{
                                    class_ = "opq-route-segment"
                                }(dstPath.generic_string())
                            );
                        }
                        return span{
                            class_ = "opq-route-segment"
                        }(currentFile.value());
                    }
                ),
                span{
                    class_ = "opq-status-text"
                }(
                    observe(fileCurrentIndex, fileCount, bytesPerSecond),
                    [this](){
                        return statusText();
                    }
                )
            ),
            div{}(
                totalProgressBar_(),
                fileProgressBar_()
            )
        );
        // clang-format on
    }

    void state(SharedData::OperationState newState) override
    {
        OperationCard::state(newState);
        if (isCompletedState())
        {
            fileProgressBar_.setZeroAsComplete();
            totalProgressBar_.setZeroAsComplete();
        }
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