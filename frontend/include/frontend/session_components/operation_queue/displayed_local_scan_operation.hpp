#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

class DisplayedLocalScanOperation : public OperationCard<DisplayedLocalScanOperation>
{
  public:
    DisplayedLocalScanOperation(
        Ids::OperationId operationId,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown
    )
        : OperationCard{
              SharedData::OperationType::LocalScan,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown)
          }
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
                div {
                    style = "margin-top: 8px; font-size: 13px; color: var(--muted);"
                }(
                    observe(totalBytes_, currentIndex_, totalScanned_).generate([this]() -> std::string {
                        return fmt::format(
                            "Scanned a total of {} items, currently at item {} ({} total).",
                            totalScanned_.value(),
                            currentIndex_.value(),
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

    std::string title() const override
    {
        return "Scanning local directory";
    }

    bool warrantsCancelConfirm() const override
    {
        return true;
    }

    std::string statusText() const override
    {
        return fmt::format(
            "status: {}, scanned {} items of size {}",
            formattedState(),
            totalScanned_.value(),
            Utility::formatBytes(totalBytes_.value(), Utility::determineOrderOfMagnitude(totalBytes_.value()))
        );
    }

  private:
    Nui::Observed<std::uint64_t> totalBytes_{0ull};
    Nui::Observed<std::uint64_t> currentIndex_{0ull};
    Nui::Observed<std::uint64_t> totalScanned_{0ull};
};