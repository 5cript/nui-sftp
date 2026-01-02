#pragma once

#include <frontend/session_components/operation_queue/operation_card.hpp>

struct DisplayedCustomAction : public OperationCard<DisplayedCustomAction>
{
    DisplayedCustomAction(
        Ids::OperationId operationId,
        std::function<void(OperationCard const& operation)> doRemoveSelf,
        std::shared_ptr<Nui::Observed<bool>> doDeletionCountdown,
        std::function<void(std::optional<Ids::OperationId> const&)> action
    )
        : OperationCard{
              SharedData::OperationType::CustomAction,
              std::move(operationId),
              std::move(doRemoveSelf),
              std::move(doDeletionCountdown),
              true
          }
        , action_{[alreadyPerformed = false,
                      action = std::move(action)](std::optional<Ids::OperationId> const& id) mutable
              {
                  if (alreadyPerformed)
                      return;
                  alreadyPerformed = true;
                  if (action)
                      action(id);
              }}
    {}

    bool warrantsCancelConfirm() const override
    {
        return false;
    }

    std::string statusText() const override
    {
        return "";
    }

    std::string title() const override
    {
        return "";
    }

    void state(SharedData::OperationState state) override
    {
        OperationCard::state(state);
        if (state == SharedData::OperationState::Completed)
            action_(operationId());
    }

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
            }()
        );
        // clang-format on
    }

  private:
    std::function<void(std::optional<Ids::OperationId> const&)> action_;
};