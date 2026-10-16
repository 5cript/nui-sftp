#pragma once

#include <backend/sftp/operation.hpp>

#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <utility>

/**
 * @brief A scan that belongs to the operation consuming its result. The consumer steps it before
 *        its own work, so cancelling, reordering or failing the consumer acts on its scan too.
 */
class ScanPhaseDriver
{
  public:
    virtual ~ScanPhaseDriver() = default;

    /**
     * @brief Steps the current scan once. Complete when every root was scanned and delivered.
     */
    virtual std::expected<Operation::WorkStatus, Operation::Error> step() = 0;

    virtual void cancel(bool adoptCancelState) = 0;
};

/**
 * @brief Scans one or more roots in order and hands each finished scan to the consumer.
 *
 * @tparam ScanT ScanOperation or LocalScanOperation.
 */
template <typename ScanT>
class ScanPhase : public ScanPhaseDriver
{
  public:
    /**
     * @brief Receives a finished scan; the consumer ejects its entries.
     */
    using DeliverFunction = std::function<void(ScanT& scan)>;

    explicit ScanPhase(DeliverFunction deliver)
        : deliver_{std::move(deliver)}
    {}

    void addRoot(std::unique_ptr<ScanT> scan)
    {
        scans_.push_back(std::move(scan));
    }

    std::expected<Operation::WorkStatus, Operation::Error> step() override
    {
        if (scans_.empty())
            return Operation::WorkStatus::Complete;

        auto& current = *scans_.front();
        auto result = current.work();
        if (!result.has_value())
            return std::unexpected(std::move(result).error());
        if (result.value() != Operation::WorkStatus::Complete)
            return Operation::WorkStatus::MoreWork;

        deliver_(current);
        scans_.pop_front();
        return scans_.empty() ? Operation::WorkStatus::Complete : Operation::WorkStatus::MoreWork;
    }

    void cancel(bool adoptCancelState) override
    {
        for (auto& scan : scans_)
            std::ignore = scan->cancel(adoptCancelState);
    }

  private:
    DeliverFunction deliver_;
    std::deque<std::unique_ptr<ScanT>> scans_{};
};

/**
 * @brief Steps @p phase and drops it once every root is delivered.
 *
 * @return MoreWork while scanning or right after the last delivery, the scan's error otherwise.
 */
template <typename PhaseT>
std::expected<Operation::WorkStatus, Operation::Error> stepScanPhase(std::unique_ptr<PhaseT>& phase)
{
    auto result = phase->step();
    if (!result.has_value())
        return result;
    if (result.value() == Operation::WorkStatus::Complete)
        phase.reset();
    return Operation::WorkStatus::MoreWork;
}
