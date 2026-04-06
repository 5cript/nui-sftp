#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

class DisplayedRenameOperation : public OperationCard<DisplayedRenameOperation>
{
  public:
    DisplayedRenameOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        std::filesystem::path sourcePath,
        std::filesystem::path destinationPath,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void()> onCompleteAction
    )
        : OperationCard{
              SharedData::OperationType::Rename,
              confirmDialog,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown),
              std::move(onCompleteAction)
          }
        , sourcePath_{std::move(sourcePath)}
        , destinationPath_{std::move(destinationPath)}
    {}

    bool warrantsCancelConfirm() const override
    {
        return false;
    }

    Nui::ElementRenderer body() const override
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::span;
        using Nui::Elements::div;

        // clang-format off
        return fragment(
            span{}(
                fmt::format(
                    "Rename: '{}' -> '{}'",
                    sourcePath_.filename().string(),
                    destinationPath_.filename().string()
                )
            ),
            div{}()
        );
        // clang-format on
    }

  private:
    std::filesystem::path sourcePath_;
    std::filesystem::path destinationPath_;
};
