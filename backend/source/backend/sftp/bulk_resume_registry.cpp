#include <backend/sftp/bulk_resume_registry.hpp>

#include <log/log.hpp>

void BulkResumeRegistry::store(Ids::OperationId const& operationId, BulkResumeEntry entry)
{
    std::lock_guard<std::mutex> lock{mutex_};
    evictExpiredAlreadyLocked();
    entry.storedAt = std::chrono::steady_clock::now();
    auto [iter, inserted] = entries_.try_emplace(operationId, std::move(entry));
    if (!inserted)
    {
        Log::warn(
            "BulkResumeRegistry: replacing existing backup for operation id '{}'",
            operationId.value()
        );
        iter->second = std::move(entry);
    }
}

std::optional<BulkResumeEntry> BulkResumeRegistry::take(Ids::OperationId const& operationId)
{
    std::lock_guard<std::mutex> lock{mutex_};
    evictExpiredAlreadyLocked();
    auto iter = entries_.find(operationId);
    if (iter == entries_.end())
        return std::nullopt;
    auto out = std::move(iter->second);
    entries_.erase(iter);
    return out;
}

void BulkResumeRegistry::discard(Ids::OperationId const& operationId)
{
    std::lock_guard<std::mutex> lock{mutex_};
    evictExpiredAlreadyLocked();
    entries_.erase(operationId);
}

std::size_t BulkResumeRegistry::size() const
{
    std::lock_guard<std::mutex> lock{mutex_};
    return entries_.size();
}

void BulkResumeRegistry::evictExpiredAlreadyLocked()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto iter = entries_.begin(); iter != entries_.end();)
    {
        if (now - iter->second.storedAt > ttl_)
        {
            Log::info(
                "BulkResumeRegistry: TTL-evicting backup for operation id '{}'",
                iter->first.value()
            );
            iter = entries_.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}
