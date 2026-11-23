#include <backend/sftp/kept_transfer_registry.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>

#include <system_error>
#include <utility>

void removeLocalPartialFiles(std::vector<std::filesystem::path> const& paths)
{
    for (auto const& path : paths)
    {
        std::error_code removeError{};
        if (std::filesystem::remove(path, removeError))
            Log::info("Removed the partial download '{}'.", Paths::nativeToUtf8(path));
        else if (removeError)
            Log::warn("Cannot remove '{}': {}", Paths::nativeToUtf8(path), Utility::errorMessage(removeError));
    }
}

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

bool KeptTransferRegistry::discardWithoutSession(Ids::OperationId const& operationId)
{
    std::optional<KeptTransfer> taken{};
    {
        const std::lock_guard lock{mutex_};
        const auto found = transfers_.find(operationId);
        if (found == transfers_.end())
            return true;
        if (!found->second.partialFiles.remote.empty())
            return false;
        taken = std::move(transfers_.extract(found).mapped());
    }
    removeLocalPartialFiles(taken->partialFiles.local);
    return true;
}

std::size_t KeptTransferRegistry::size() const
{
    const std::lock_guard lock{mutex_};
    return transfers_.size();
}
