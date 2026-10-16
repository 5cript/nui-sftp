#include <frontend/session_components/local_operation_router.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/notifications.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <utility/language.hpp>

#include <log/log.hpp>

#include <nui/frontend/utility/val_conversion.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <string>
#include <utility>

namespace
{
    /**
     * @brief Tells the user how a delete ended that no queue showed.
     */
    void notifyUnshownCompletion(SharedData::OperationCompleted const& completed, std::string const& directory)
    {
        using enum SharedData::OperationCompletionReason;
        switch (completed.reason)
        {
            case Completed:
            {
                if (completed.failedEntries.empty())
                    return Notifications::success(
                        fmt::format(fmt::runtime(language->get("operationQueue", "localDeleteDone")), directory)
                    );
                return Notifications::warning(
                    fmt::format(fmt::runtime(language->get("operationQueue", "localDeletePartlyDone")), directory)
                );
            }
            case Failed:
            {
                return Notifications::error(
                    fmt::format(
                        fmt::runtime(language->get("operationQueue", "localDeleteFailed")),
                        directory,
                        completed.error ? completed.error->toString() : language->get("operationQueue", "unknownError")
                    )
                );
            }
            case Canceled:
                return;
        }
    }
}

LocalOperationRouter& LocalOperationRouter::instance()
{
    static LocalOperationRouter router;
    return router;
}

LocalOperationRouter::LocalOperationRouter()
{
    registrations_.push_back(
        Nui::RpcClient::autoRegisterFunction(
            "LocalOperationQueue::onOperationAdded",
            [this](SharedData::OperationAdded const& added)
            {
                onOperationAdded(added);
            }
        )
    );
    registrations_.push_back(
        Nui::RpcClient::autoRegisterFunction(
            "LocalOperationQueue::onScanProgress",
            [this](SharedData::ScanProgress const& progress)
            {
                onScanProgress(progress);
            }
        )
    );
    registrations_.push_back(
        Nui::RpcClient::autoRegisterFunction(
            "LocalOperationQueue::onDeleteProgress",
            [this](SharedData::BulkDeleteProgress const& progress)
            {
                onDeleteProgress(progress);
            }
        )
    );
    registrations_.push_back(
        Nui::RpcClient::autoRegisterFunction(
            "LocalOperationQueue::onOperationCompleted",
            [this](Nui::val val)
            {
                onOperationCompleted(std::move(val));
            }
        )
    );
}

void LocalOperationRouter::attach(OperationQueue& queue)
{
    queues_.push_back(&queue);
    for (auto& [operationId, routed] : routed_)
    {
        if (routed.host)
            continue;
        routed.host = &queue;
        show(routed, queue);
    }
}

void LocalOperationRouter::detach(OperationQueue& queue)
{
    std::erase(queues_, &queue);
    auto* const successor = newestQueue();
    for (auto& [operationId, routed] : routed_)
    {
        if (routed.host != &queue)
            continue;
        routed.host = successor;
        if (successor)
            show(routed, *successor);
    }
}

void LocalOperationRouter::host(Ids::OperationId const& operationId, OperationQueue& queue)
{
    routed_[operationId].host = &queue;
}

void LocalOperationRouter::forget(Ids::OperationId const& operationId)
{
    routed_.erase(operationId);
}

void LocalOperationRouter::cancelHostedBy(OperationQueue const& queue)
{
    std::erase_if(
        routed_,
        [&queue](auto const& entry)
        {
            auto const& [operationId, routed] = entry;
            if (routed.host != &queue)
                return false;
            Nui::RpcClient::callWithBackChannel(
                "LocalOperationQueue::cancel",
                [operationId](SharedData::ErrorOrSuccess<> const& result)
                {
                    if (!result)
                        Log::warn(
                            "Failed to cancel the local operation '{}': {}", operationId.value(), result.error.value()
                        );
                },
                operationId
            );
            return true;
        }
    );
}

void LocalOperationRouter::onOperationAdded(SharedData::OperationAdded const& added)
{
    auto [found, inserted] = routed_.try_emplace(added.operationId);
    auto& routed = found->second;
    if (inserted)
        routed.host = newestQueue();
    routed.added = added;
    if (routed.host)
        routed.host->onOperationAdded(added);
}

void LocalOperationRouter::onScanProgress(SharedData::ScanProgress const& progress)
{
    const auto found = routed_.find(progress.operationId);
    if (found == routed_.end())
        return;
    found->second.scanProgress = progress;
    if (found->second.host)
        found->second.host->onScanProgress(progress);
}

void LocalOperationRouter::onDeleteProgress(SharedData::BulkDeleteProgress const& progress)
{
    const auto found = routed_.find(progress.operationId);
    if (found == routed_.end())
        return;
    found->second.deleteProgress = progress;
    if (found->second.host)
        found->second.host->onDeleteProgress(progress);
}

void LocalOperationRouter::onOperationCompleted(Nui::val val)
{
    SharedData::OperationCompleted completed{};
    try
    {
        Nui::convertFromVal(val, completed);
    }
    catch (std::exception const& exception)
    {
        Log::error("Failed to read the completion of a local operation: {}", exception.what());
        return;
    }

    auto routed = routed_.extract(completed.operationId);
    if (routed.empty())
    {
        Log::info("Local operation '{}' ended without a card.", completed.operationId.value());
        return;
    }
    if (auto* const host = routed.mapped().host; host)
        return host->onOperationCompleted(std::move(completed));

    auto const& added = routed.mapped().added;
    notifyUnshownCompletion(completed, added && added->localPath ? added->localPath->toUtf8() : std::string{});
}

void LocalOperationRouter::show(Routed& routed, OperationQueue& queue)
{
    if (!routed.added)
        return;
    queue.onOperationAdded(*routed.added);
    if (routed.scanProgress)
        queue.onScanProgress(*routed.scanProgress);
    if (routed.deleteProgress)
        queue.onDeleteProgress(*routed.deleteProgress);
}

OperationQueue* LocalOperationRouter::newestQueue() const
{
    return queues_.empty() ? nullptr : queues_.back();
}
