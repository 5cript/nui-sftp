#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

class DisplayedScanOperation : public OperationCard<DisplayedScanOperation>
{
  public:
    DisplayedScanOperation(
        Ids::OperationId operationId,
        std::filesystem::path remotePath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown
    )
        : OperationCard{
              SharedData::OperationType::Scan,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown)
          }
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
            class_ = "opq-body"
        }(
            span{}(
                fmt::format("Scan '{}' - ", remotePath_.string())
            ),
            span{}(
                observe(totalBytes_, currentIndex_, totalScanned_).generate([this]() -> std::string {
                    return fmt::format(
                        "{}/{} items ({})",
                        currentIndex_.value(),
                        totalScanned_.value(),
                        Utility::formatBytes(totalBytes_.value(), Utility::determineOrderOfMagnitude(totalBytes_.value())));
                })
            )
        );
        // clang-format on
    }

    void setProgress(std::uint64_t totalBytes, std::uint64_t currentIndex, std::uint64_t totalScanned)
    {
        totalBytes_ = totalBytes;
        currentIndex_ = currentIndex;
        totalScanned_ = totalScanned;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    bool warrantsCancelConfirm() const override
    {
        return true;
    }

  private:
    std::filesystem::path remotePath_;
    Nui::Observed<std::uint64_t> totalBytes_{0ull};
    Nui::Observed<std::uint64_t> currentIndex_{0ull};
    Nui::Observed<std::uint64_t> totalScanned_{0ull};
};