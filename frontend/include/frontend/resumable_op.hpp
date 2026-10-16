#pragma once

#include <ids/ids.hpp>
#include <paths/paths.hpp>

#include <variant>

/**
 * @brief Operations that were in flight when the SSH transport died, re-enqueued against the new session with
 *        tryContinue=true. Scans, sync operations, archives and local deletes are not resumable.
 */
namespace Resumable
{
    struct Download
    {
        Paths::RemotePath source{};
        Paths::LocalPath destination{};
        /** @brief What the interrupted transfer was allowed, a resume must not replace more. */
        bool allowOverwrite{false};
    };

    struct Upload
    {
        Paths::LocalPath source{};
        Paths::RemotePath destination{};
        bool allowOverwrite{false};
    };

    struct Rename
    {
        Paths::RemotePath source{};
        Paths::RemotePath destination{};
    };

    /**
     * @brief A delete of one remote item.
     */
    struct Delete
    {
        Paths::RemotePath target{};
        /** @brief Only a delete that scanned a directory resumes with its content. */
        bool recursive{false};
    };

    /**
     * @brief A bulk card. The new session passes its id to SessionManager::adoptBulkResume and the backend
     *        re-issues the bulk from its own backup.
     */
    struct Bulk
    {
        Ids::OperationId operationId{};
    };
}

using ResumableOp =
    std::variant<Resumable::Download, Resumable::Upload, Resumable::Rename, Resumable::Delete, Resumable::Bulk>;
