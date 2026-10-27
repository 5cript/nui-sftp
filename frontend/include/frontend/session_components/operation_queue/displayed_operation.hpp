#pragma once

#include <frontend/session_components/operation_queue/operation_card_interface.hpp>

#include <ids/ids.hpp>
#include <shared_data/file_operations/operation_added.hpp>

#include <optional>

struct DisplayedOperation
{
  public:
    DisplayedOperation(
        Ids::OperationId operationId,
        SharedData::OperationType type,
        std::unique_ptr<OperationCardInterface> card,
        std::optional<SharedData::OperationAdded> added = std::nullopt
    )
        : operationId_{std::move(operationId)}
        , type_{type}
        , card_{std::move(card)}
        , added_{std::move(added)}
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

    void setError(SharedData::OperationError const& error)
    {
        card_->setError(error);
    }

    template <typename T>
    T* getCardSpecifically()
    {
        return dynamic_cast<T*>(card_.get());
    }

    void failedEntries(std::vector<std::pair<std::string, SharedData::OperationError>> entries)
    {
        if (auto* specificCard = getCardSpecifically<OperationCardInterface>())
        {
            specificCard->failedEntries(std::move(entries));
        }
    }

    std::optional<ResumableOp> resumableDescriptor() const
    {
        return card_->resumableDescriptor();
    }

    bool keptPartialFiles() const
    {
        return card_->keptPartialFiles();
    }

    /**
     * @brief The card for the snapshot of a reconnect while it waits to continue or discard its kept partial files.
     */
    std::optional<Resumable::Kept> keptDescriptor() const
    {
        if (!added_ || !card_->keptPartialFiles())
            return std::nullopt;
        const auto [progress, maximum] = card_->barPosition();
        return Resumable::Kept{.added = *added_, .progress = progress, .maximum = maximum};
    }

    OperationCardInterface& card()
    {
        return *card_;
    }

  private:
    Ids::OperationId operationId_;
    SharedData::OperationType type_;
    std::unique_ptr<OperationCardInterface> card_;
    /** @brief What the card was made from, so a reconnect can make it again. */
    std::optional<SharedData::OperationAdded> added_;
};