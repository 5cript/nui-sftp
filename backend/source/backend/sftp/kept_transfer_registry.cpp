#include <backend/sftp/kept_transfer_registry.hpp>

#include <utility>

void KeptTransferRegistry::store(Ids::OperationId const& operationId, KeptTransfer transfer)
{
    const std::lock_guard lock{mutex_};
    transfers_.insert_or_assign(operationId, std::move(transfer));
}

std::optional<KeptTransfer> KeptTransferRegistry::take(Ids::OperationId const& operationId)
{
    const std::lock_guard lock{mutex_};
    auto found = transfers_.extract(operationId);
    if (found.empty())
        return std::nullopt;
    return std::move(found.mapped());
}

std::size_t KeptTransferRegistry::size() const
{
    const std::lock_guard lock{mutex_};
    return transfers_.size();
}
