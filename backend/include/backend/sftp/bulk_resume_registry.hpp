#pragma once

#include <ids/ids.hpp>
#include <shared_data/file_operations/bulk_delete_request.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/file_operations/operation_mode.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <variant>

/**
 * @brief The request a backup re-issues, its type picks the addBulk* path.
 */
using BulkResumeRequest =
    std::variant<SharedData::BulkDownloadRequest, SharedData::BulkUploadRequest, SharedData::BulkDeleteRequest>;

/**
 * @brief Entries and bytes of a bulk finished on an earlier connection. Its resumed card counts them as done, so it
 *        shows the whole bulk instead of a smaller one.
 */
struct BulkResumeProgress
{
    std::uint64_t entries{0};
    std::uint64_t bytes{0};
};

/**
 * @brief Carries the state needed to re-enqueue a bulk operation against a
 *        replacement Session after a seamless reconnect.
 *
 *        One entry per frontend card. A transfer entry holds either the files
 *        of one bulk card or a single directory, so adopting it re-issues
 *        exactly that card under its own id.
 */
struct BulkResumeEntry
{
    BulkResumeRequest request;
    /** @brief What the bulk finished before the backup, left out of @ref request. */
    BulkResumeProgress finished{};
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
