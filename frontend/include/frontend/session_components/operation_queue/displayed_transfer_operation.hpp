
#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>
#include <frontend/components/svg/arrow_right.hpp>

#include <utility/format_bytes.hpp>

class DisplayedTransferOperation : public OperationCard<DisplayedTransferOperation>
{
  public:
    DisplayedTransferOperation(
        Ids::OperationId operationId,
        SharedData::OperationType type,
        ConfirmDialog& confirmDialog,
        long long max,
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
        , progressBar_{{
              .height = std::string{progressHeight},
              .min = 0,
              .max = max,
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
        const bool isDownloadOrientation =
            type_ == SharedData::OperationType::Download ||
            type_ == SharedData::OperationType::ArchiveDownload;
        const auto& srcPath = isDownloadOrientation ? remotePath_ : localPath_;
        const auto& dstPath = isDownloadOrientation ? localPath_ : remotePath_;
        const bool hasBothPaths = !srcPath.empty() && !dstPath.empty();
        const auto& solePath = srcPath.empty() ? dstPath : srcPath;

        Nui::ElementRenderer routeDiv;
        if (hasBothPaths)
        {
            routeDiv = div{
                class_ = "opq-transfer-route",
                alt = fmt::format("{} \u2192 {}", srcPath.generic_string(), dstPath.generic_string())
            }(
                span{class_ = "opq-route-segment"}(srcPath.generic_string()),
                span{class_ = "opq-route-arrow"}(Svgs::arrowRight()),
                span{class_ = "opq-route-segment"}(dstPath.generic_string())
            );
        }
        else
        {
            routeDiv = div{
                class_ = "opq-transfer-route",
                alt = solePath.generic_string()
            }(
                span{class_ = "opq-route-segment"}(solePath.generic_string())
            );
        }

        return div{
            class_ = "opq-body opq-single"
        }(
            div{}(
                std::move(routeDiv),
                div{
                    class_ = "opq-bytes-per-second"
                }(
                    observe(bytesPerSecond_).generate(
                        [](std::make_signed_t<std::size_t> bytesPerSecond)
                        {
                            return fmt::format("{}/s", Utility::formatBytes(bytesPerSecond, Utility::determineOrderOfMagnitude(bytesPerSecond)));
                        }
                    )
                )
            ),
            progressBar_()
        );
        // clang-format on
    }

    void setProgress(SharedData::TransferProgress progress)
    {
        progressBar_.max(progress.max);
        progressBar_.setProgress(progress.current - progress.min);
        bytesPerSecond_ = progress.bytesPerSecond;
    }

    bool warrantsCancelConfirm() const override
    {
        return !isCompletedState() && progressBar_.max() > 10'000'000; // 10 MB
    }

    void state(SharedData::OperationState newState) override
    {
        OperationCard::state(newState);
        if (isCompletedState())
            progressBar_.setZeroAsComplete();
    }

    std::optional<ResumableOp> resumableDescriptor() const override
    {
        if (isCompletedState())
            return std::nullopt;
        ResumableOp out;
        if (type_ == SharedData::OperationType::Download)
        {
            out.kind = ResumableOp::Kind::Download;
            out.src = remotePath_;
            out.dst = localPath_;
        }
        else
        {
            out.kind = ResumableOp::Kind::Upload;
            out.src = localPath_;
            out.dst = remotePath_;
        }
        out.allowOverwrite = true;
        return out;
    }

  private:
    Components::ProgressBar progressBar_;
    Nui::Observed<std::make_signed_t<std::size_t>> bytesPerSecond_{0};
    std::filesystem::path localPath_;
    std::filesystem::path remotePath_;
};