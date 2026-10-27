#pragma once

#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/sftp/operation.hpp>
#include <ids/ids.hpp>
#include <paths/local_path.hpp>
#include <paths/remote_path.hpp>
#include <shared_data/file_operations/operation_type.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <variant>

/**
 * @brief A single file transfer a continuation queues again under its id.
 */
struct KeptSingleTransfer
{
    /** @brief Download or Upload. */
    SharedData::OperationType type{SharedData::OperationType::Download};
    Paths::LocalPath localPath{};
    Paths::RemotePath remotePath{};
    bool allowOverwrite{false};
    bool createMissingDirectories{false};
};

/**
 * @brief How a continuation queues a kept transfer again: a bulk from its backup, a single transfer anew.
 */
using KeptContinuation = std::variant<BulkResumeEntry, KeptSingleTransfer>;

/**
 * @brief A transfer the user canceled that kept its partial files.
 */
struct KeptTransfer
{
    Operation::KeptPartialFiles partialFiles{};
    /** @brief Nothing when it cannot continue, it can only be discarded then. */
    std::optional<KeptContinuation> continuation{};
};

/**
 * @brief Process-lifetime store of the transfers canceled keeping their partial files, keyed by operation id. Owned by
 *        SessionManager and shared by the queues of its sessions, so a card can continue or discard after a reconnect
 *        replaced the session that canceled it. Entries stay until a continue or discard takes them. Thread-safe.
 */
class KeptTransferRegistry
{
  public:
    KeptTransferRegistry() = default;
    ~KeptTransferRegistry() = default;
    KeptTransferRegistry(KeptTransferRegistry const&) = delete;
    KeptTransferRegistry& operator=(KeptTransferRegistry const&) = delete;
    KeptTransferRegistry(KeptTransferRegistry&&) = delete;
    KeptTransferRegistry& operator=(KeptTransferRegistry&&) = delete;

    /**
     * @brief Stores or replaces the kept transfer @p operationId.
     */
    void store(Ids::OperationId const& operationId, KeptTransfer transfer);

    /**
     * @brief Takes the kept transfer @p operationId out, nothing when none is stored.
     */
    std::optional<KeptTransfer> take(Ids::OperationId const& operationId);

    std::size_t size() const;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<Ids::OperationId, KeptTransfer, Ids::IdHash> transfers_;
};
