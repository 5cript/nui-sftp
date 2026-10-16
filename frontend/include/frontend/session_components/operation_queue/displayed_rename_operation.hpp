#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>
#include <utility/language.hpp>

class DisplayedRenameOperation : public OperationCard<DisplayedRenameOperation>
{
  public:
    DisplayedRenameOperation(
        Ids::OperationId operationId,
        ConfirmDialog& confirmDialog,
        Paths::RemotePath sourcePath,
        Paths::RemotePath destinationPath,
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

    std::optional<ResumableOp> resumableDescriptor() const override
    {
        if (isCompletedState())
            return std::nullopt;
        return Resumable::Rename{.source = sourcePath_, .destination = destinationPath_};
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
                    fmt::runtime(language->get("operationQueue", "renameCard")),
                    nameOf(sourcePath_),
                    nameOf(destinationPath_)
                )
            ),
            div{}()
        );
        // clang-format on
    }

  private:
    static std::string nameOf(Paths::RemotePath const& path)
    {
        if (auto name = path.filename(); name)
            return name->value();
        return path.value();
    }

  private:
    Paths::RemotePath sourcePath_;
    Paths::RemotePath destinationPath_;
};
