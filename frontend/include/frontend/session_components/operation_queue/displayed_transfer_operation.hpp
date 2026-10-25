
#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>
#include <frontend/session_components/operation_queue/scan_phase_status.hpp>
#include <frontend/components/svg/arrow_right.hpp>

#include <script-nui-components/progress_bar.hpp>
#include <utility/format_bytes.hpp>

class DisplayedTransferOperation
    : public OperationCard<DisplayedTransferOperation>
    , public ScanningCard
{
  public:
    DisplayedTransferOperation(
        Ids::OperationId operationId,
        SharedData::OperationType type,
        ConfirmDialog& confirmDialog,
        long long max,
        Paths::LocalPath localPath,
        Paths::RemotePath remotePath,
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
        , progressBar_{{
              .height = std::string{progressHeight},
              .minimum = 0,
              .maximum = max,
              .texts = {.percent = "{:.0f}%"},
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
        const auto remoteText = remotePath_.value();
        const auto localText = localPath_.toUtf8();
        const auto& srcPath = isDownloadOrientation ? remoteText : localText;
        const auto& dstPath = isDownloadOrientation ? localText : remoteText;
        const bool hasBothPaths = !srcPath.empty() && !dstPath.empty();
        const auto& solePath = srcPath.empty() ? dstPath : srcPath;

        Nui::ElementRenderer routeDiv;
        if (hasBothPaths)
        {
            routeDiv = div{
                class_ = "opq-transfer-route",
                alt = fmt::format("{} \u2192 {}", srcPath, dstPath)
            }(
                span{class_ = "opq-route-segment"}(srcPath),
                span{class_ = "opq-route-arrow"}(Svgs::arrowRight()),
                span{class_ = "opq-route-segment"}(dstPath)
            );
        }
        else
        {
            routeDiv = div{
                class_ = "opq-transfer-route",
                alt = solePath
            }(
                span{class_ = "opq-route-segment"}(solePath)
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
                    observe(bytesPerSecond_, scanPhase_.scanning, scanPhase_.totalScanned),
                    [this]()
                    {
                        if (scanPhase_.scanning.value() && !isCompletedState())
                            return scanPhase_.text();
                        const auto bytesPerSecond = bytesPerSecond_.value();
                        return fmt::format("{}/s", Utility::formatBytes(bytesPerSecond, Utility::determineOrderOfMagnitude(bytesPerSecond)));
                    }
                )
            ),
            progressBar_()
        );
        // clang-format on
    }

    void setProgress(SharedData::TransferProgress progress)
    {
        scanPhase_.end();
        progressBar_.setTextMode(ScriptNuiComponents::ProgressBarTextMode::Bytes);
        progressBar_.setMaximum(progress.max);
        progressBar_.setProgress(progress.current - progress.min);
        bytesPerSecond_ = progress.bytesPerSecond;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    bool warrantsCancelConfirm() const override
    {
        return !isCompletedState() && progressBar_.maximum() > 10'000'000; // 10 MB
    }

    void state(SharedData::OperationState newState) override
    {
        OperationCard::state(newState);
        if (isCompletedState())
        {
            progressBar_.setTextMode(ScriptNuiComponents::ProgressBarTextMode::Bytes);
            progressBar_.setZeroAsComplete();
        }
    }

    /**
     * @brief Whether the transfer may replace its destination, so a resume asks for no more.
     */
    void allowOverwrite(bool allow)
    {
        allowOverwrite_ = allow;
    }

    std::optional<ResumableOp> resumableDescriptor() const override
    {
        if (isCompletedState())
            return std::nullopt;
        if (type_ == SharedData::OperationType::Download)
            return Resumable::Download{
                .source = remotePath_, .destination = localPath_, .allowOverwrite = allowOverwrite_
            };
        if (type_ == SharedData::OperationType::Upload)
            return Resumable::Upload{.source = localPath_, .destination = remotePath_, .allowOverwrite = allowOverwrite_};
        // An archive card does not know the entries it packs, so it cannot be redone.
        return std::nullopt;
    }

  private:
    /** @brief Shows "0%" until the first progress or the end, then bytes. */
    ScriptNuiComponents::ProgressBar progressBar_;
    Nui::Observed<std::make_signed_t<std::size_t>> bytesPerSecond_{0};
    /** @brief Empty for an archive upload. */
    Paths::LocalPath localPath_;
    bool allowOverwrite_{false};
    /** @brief Unset for an archive download. */
    Paths::RemotePath remotePath_;
};