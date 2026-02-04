
#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

#include <utility/format_bytes.hpp>

class DisplayedTransferOperation : public OperationCard<DisplayedTransferOperation>
{
  public:
    DisplayedTransferOperation(
        Ids::OperationId operationId,
        SharedData::OperationType type,
        long long max,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction
    )
        : OperationCard{
              type,
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
        return div{
            class_ = "opq-body opq-single"
        }(
            div{}(
                span{
                    style = "flex-grow: 1"
                }(
                    fmt::format("{} -> {}", remotePath_.generic_string(), localPath_.generic_string())
                ),
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

  private:
    Components::ProgressBar progressBar_;
    Nui::Observed<std::make_signed_t<std::size_t>> bytesPerSecond_{0};
    std::filesystem::path localPath_;
    std::filesystem::path remotePath_;
};