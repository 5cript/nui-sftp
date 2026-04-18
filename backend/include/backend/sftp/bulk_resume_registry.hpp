#pragma once

#include <ids/ids.hpp>
#include <shared_data/file_operations/bulk_add_request.hpp>
#include <shared_data/file_operations/operation_mode.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>

/**
 * @brief Carries the state needed to re-enqueue a bulk operation against a
 *        replacement Session after a seamless reconnect.
 *
 *        The kind tracks which addBulk* path to take; the request mirrors
 *        the original BulkAddRequest minus already-completed entries (and
 *        the head entry that was mid-transfer when the connection died, on
 *        the assumption that the per-file resume path is not byte-accurate
 *        across a reconnect).
 */
struct BulkResumeEntry
{
    enum class Kind
    {
        BulkDownload,
        BulkUpload,
        BulkDelete,
    };

    Kind kind;
    SharedData::BulkAddRequest request;
    std::chrono::steady_clock::time_point storedAt{std::chrono::steady_clock::now()};
};

/**
 * @brief Process-lifetime store for bulk-operation backups produced when a
 *        Session shuts down, keyed by the bulk's frontend-allocated
 *        OperationId.  A replacement Session reads entries back via take()
 *        and re-enqueues them; entries the user never adopts are removed
 *        either by an explicit discard() (e.g. on tab-close) or by the TTL
 *        eviction sweep performed lazily on each mutation.
 *
 *        Owned by SessionManager.  Thread-safe; the registry is touched
 *        from the SSH strand, the SessionManager strand, and RPC reply
 *        callbacks.
 */
class BulkResumeRegistry
{
  public:
    static constexpr std::chrono::minutes defaultTtl{10};

    BulkResumeRegistry() = default;
    explicit BulkResumeRegistry(std::chrono::seconds ttl) : ttl_{ttl} {}

    BulkResumeRegistry(BulkResumeRegistry const&) = delete;
    BulkResumeRegistry& operator=(BulkResumeRegistry const&) = delete;
    BulkResumeRegistry(BulkResumeRegistry&&) = delete;
    BulkResumeRegistry& operator=(BulkResumeRegistry&&) = delete;

    /** @brief Stores or replaces a backup keyed by @p operationId. */
    void store(Ids::OperationId const& operationId, BulkResumeEntry entry);

    /** @brief Pops the backup for @p operationId, if any.  Used by the
     *         adopt path when re-enqueueing into a new Session. */
    std::optional<BulkResumeEntry> take(Ids::OperationId const& operationId);

    /** @brief Drops the backup for @p operationId without consuming it.
     *         Used by the cleanup signal fired from the abandoned-session
     *         destructor. */
    void discard(Ids::OperationId const& operationId);

    /** @brief Test-/debug-only inspection of the current size. */
    std::size_t size() const;

  private:
    /** @brief Caller must hold @c mutex_. */
    void evictExpiredAlreadyLocked();

    mutable std::mutex mutex_;
    std::unordered_map<Ids::OperationId, BulkResumeEntry, Ids::IdHash> entries_;
    std::chrono::seconds ttl_{defaultTtl};
};
