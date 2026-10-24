#include <frontend/session_components/file_tracking/translated_texts.hpp>
#include <utility/language.hpp>

namespace FileTracking
{
    FileTrackingTexts translatedTexts()
    {
        return {
            .counts =
                {
                    .conflictOne = language->get("fileTracking", "conflictCountOne"),
                    .conflicts = language->get("fileTracking", "conflictCount"),
                    .failed = language->get("fileTracking", "failedCount"),
                },
            .stopSides =
                {
                    .stoppedByUser = language->get("fileTracking", "sideStoppedByUser"),
                    .overlap = language->get("fileTracking", "sideOverlap"),
                    .copyGone = language->get("fileTracking", "sideCopyGone"),
                    .connectionLost = language->get("fileTracking", "sideConnectionLost"),
                },
            .statuses =
                {
                    .syncing = language->get("fileTracking", "groupSyncing"),
                    .upToDate = language->get("fileTracking", "statusUpToDate"),
                    .stopped = language->get("fileTracking", "groupStopped"),
                },
            .stopReasons =
                {
                    .stoppedByUser = language->get("fileTracking", "stopReasonStoppedByUser"),
                    .overlap = language->get("fileTracking", "stopReasonOverlap"),
                    .overlapUnknown = language->get("fileTracking", "stopReasonOverlapUnknown"),
                    .copyGone = language->get("fileTracking", "stopReasonCopyGone"),
                    .connectionLost = language->get("fileTracking", "stopReasonConnectionLost"),
                },
            .failureReasons =
                {
                    .lockedLocally = language->get("fileTracking", "reasonLockedLocally"),
                    .permissionDeniedLocally = language->get("fileTracking", "reasonPermissionDeniedLocally"),
                    .unreadableLocally = language->get("fileTracking", "reasonUnreadableLocally"),
                    .permissionDeniedOnServer = language->get("fileTracking", "reasonPermissionDeniedOnServer"),
                    .noSpaceOnServer = language->get("fileTracking", "reasonNoSpaceOnServer"),
                    .serverRefused = language->get("fileTracking", "reasonServerRefused"),
                    .connectionProblem = language->get("fileTracking", "reasonConnectionProblem"),
                    .notQueued = language->get("fileTracking", "reasonNotQueued"),
                    .unknown = language->get("fileTracking", "reasonUnknown"),
                },
            .conflictLines = {
                .changedOnServer = language->get("fileTracking", "conflictChangedOnServer"),
                .deletedOnServer = language->get("fileTracking", "conflictDeletedOnServer"),
                .createdOnServer = language->get("fileTracking", "conflictCreatedOnServer"),
                .timeUnknown = language->get("fileTracking", "timeUnknown"),
            },
        };
    }
}
