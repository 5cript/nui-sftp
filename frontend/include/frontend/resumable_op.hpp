#pragma once

#include <ids/ids.hpp>

#include <filesystem>

/**
 * @brief Description of an operation that was in-flight when the SSH transport
 *        died and that we want to re-enqueue against the new session with
 *        tryContinue=true.  Scan/sync operations are not resumable and are
 *        omitted from the snapshot.
 */
struct ResumableOp
{
    enum class Kind
    {
        Download,
        Upload,
        Delete,
        Rename,
        BulkDownload,
        BulkUpload,
        BulkDelete,
    };

    Kind kind;
    std::filesystem::path src;
    std::filesystem::path dst;
    bool recursive{false};
    bool allowOverwrite{false};
    bool createMissingDirectories{false};

    /**
     * @brief Backend-side resume key for bulk kinds.  The new session
     *        passes this id to SessionManager::adoptBulkResume so the
     *        backend can re-issue the bulk from its own backup of the
     *        original entry list.  Empty / invalid for single-file kinds.
     */
    Ids::OperationId operationId{};
};
