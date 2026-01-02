#pragma once

#include <frontend/session_components/operation_queue/operation_card_interface.hpp>

#include <ids/ids.hpp>

struct DisplayedOperation
{
  public:
    DisplayedOperation(
        Ids::OperationId operationId,
        SharedData::OperationType type,
        std::unique_ptr<OperationCardInterface> card
    )
        : operationId_{std::move(operationId)}
        , type_{type}
        , card_{std::move(card)}
    {}

    Nui::ElementRenderer operator()()
    {
        return (*card_)();
    }

    // for the map key
    Ids::OperationId key() const
    {
        return operationId_;
    }

    SharedData::OperationState state() const
    {
        return card_->state();
    }

    void state(SharedData::OperationState newState)
    {
        card_->state(newState);
        if (card_->isCompletedState())
            card_->completionTime(std::chrono::steady_clock::now());
    }

    bool isCompletedState() const
    {
        return card_->isCompletedState();
    }

    std::chrono::steady_clock::time_point completionTime() const
    {
        return card_->completionTime();
    }

    SharedData::OperationType type() const
    {
        return type_;
    }

    template <typename T>
    T* getCardSpecifically()
    {
        return dynamic_cast<T*>(card_.get());
    }

  private:
    Ids::OperationId operationId_;
    SharedData::OperationType type_;
    std::unique_ptr<OperationCardInterface> card_;
};