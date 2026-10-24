#pragma once

#include <backend/file_tracking/tracker_remote.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <paths/paths.hpp>

#include <vector>

namespace FileTracking
{
    /**
     * @brief Why an upload that ended with @p error failed. A local problem of the source comes first, then a lost
     *        connection, then the server's answer.
     */
    UploadFailureInfo failureInfoOf(SharedData::OperationError const& error);

    /**
     * @brief Why a bulk upload left out the entry @p failure tells of.
     */
    UploadFailureInfo failureInfoOf(SharedData::IndexedFailure const& failure);

    /**
     * @brief The entries below the directory @p directory that its upload, which ended as @p completed, left out. An
     *        entry whose name is no local path is told as the directory itself.
     */
    std::vector<FailedUploadEntry>
    failedEntriesOf(SharedData::OperationCompleted const& completed, Paths::LocalPath const& directory);

    /**
     * @brief How the single upload of @p source ended as @p completed, without what the server has at its destination
     *        and the files it sent.
     * @param isDirectory Whether @p source is a directory, its left out entries are told then.
     */
    UploadEnd
    uploadEndOf(SharedData::OperationCompleted const& completed, Paths::LocalPath const& source, bool isDirectory);

    /**
     * @brief How each upload of a bulk of files ended as the bulk @p completed, by the index of its source in
     *        @p sources, without what the server has at its destination and the files it sent. A source that was
     *        gone when its turn came ends as cancelled, its deletion is mirrored apart.
     */
    std::vector<UploadEnd>
    entryEndsOf(SharedData::OperationCompleted const& completed, std::vector<Paths::LocalPath> const& sources);

    /**
     * @brief Whether the server is asked what it has at the destination of an upload that ended as @p ended: after a
     *        completed upload of anything but a directory, and after one whose destination changed on the server.
     */
    bool asksDestinationState(UploadEnd const& ended, bool isDirectory);
}
