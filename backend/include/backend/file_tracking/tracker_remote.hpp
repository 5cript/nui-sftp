#pragma once

#include <backend/file_tracking/tracking_state.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace FileTracking
{
    /**
     * @brief What a Tracker last knew the server to have at the destination of an upload.
     */
    struct ExpectedDestination
    {
        /** @brief The file the server had there, nothing when it had none. */
        std::optional<ServerState> file{};
    };

    /**
     * @brief An upload a Tracker queues, it replaces what the server has at the destination.
     */
    struct UploadRequest
    {
        Paths::LocalPath source;
        Paths::RemotePath destination;
        /** @brief Refresh the remote listing once it is done. */
        bool refreshListing{false};
        /** @brief Create the directories above the destination that the server lacks. */
        bool createMissingDirectories{false};
        /**
         * @brief Replace a regular file's destination only while the server still has what was expected there, see
         *        UploadEnd::changedOnServer. Unset, whatever it has is replaced.
         */
        std::optional<ExpectedDestination> expectedDestination{};
    };

    /**
     * @brief A local file an upload sent, in the state it had from before it was read until the upload ended.
     */
    struct SentFile
    {
        Paths::LocalPath path;
        SourceState state;
    };

    /**
     * @brief How a queued upload ended.
     */
    struct UploadEnd
    {
        SharedData::OperationCompletionReason reason{SharedData::OperationCompletionReason::Failed};
        /** @brief The destination no longer held the request's expectedDestination, nothing replaced it. */
        bool changedOnServer{false};
        /**
         * @brief What the server has at the destination after a completed upload of anything but a directory, nothing
         *        when it has nothing there or could not be asked.
         */
        std::optional<ServerState> destinationState{};
        /**
         * @brief The regular files a completed upload sent unchanged, those below a directory too. One missing here
         *        may still have gone to the server.
         */
        std::vector<SentFile> sentFiles{};
    };

    /**
     * @brief The server side of a Tracker: the operation queue of its session, whose operations go ahead of the
     *        queue, and the server's files. Called on the strand of the session.
     */
    class TrackerRemote
    {
      public:
        /**
         * @brief Called once on the strand when a queued operation ended.
         */
        using OnEnded = std::function<void(SharedData::OperationCompletionReason reason)>;

        /**
         * @brief Called once on the strand when a queued upload ended.
         */
        using OnUploadEnded = std::function<void(UploadEnd const& ended)>;

        TrackerRemote() = default;
        virtual ~TrackerRemote() = default;
        TrackerRemote(TrackerRemote const&) = delete;
        TrackerRemote& operator=(TrackerRemote const&) = delete;
        TrackerRemote(TrackerRemote&&) = delete;
        TrackerRemote& operator=(TrackerRemote&&) = delete;

        /**
         * @brief What the server has at @p path now, nothing when it has nothing there.
         * @return An error when the server could not be asked.
         */
        virtual std::expected<std::optional<ServerState>, std::string> stat(Paths::RemotePath const& path) = 0;

        /**
         * @brief What the server has at @p path itself, a symbolic link not followed, see stat().
         */
        virtual std::expected<std::optional<ServerState>, std::string> linkStat(Paths::RemotePath const& path) = 0;

        /**
         * @brief Queues the download of @p source to @p destination, which must not exist yet.
         */
        virtual std::expected<void, std::string> download(
            Paths::RemotePath const& source,
            Paths::LocalPath const& destination,
            std::uint64_t sizeBytes,
            OnEnded onEnded
        ) = 0;

        /**
         * @brief Queues the upload, a source that is gone by the time it runs completes it without a transfer: its
         *        deletion is mirrored apart.
         */
        virtual std::expected<void, std::string> upload(UploadRequest const& request, OnUploadEnded onEnded) = 0;

        /**
         * @brief Queues the rename of @p source to @p destination, which never replaces anything.
         */
        virtual std::expected<void, std::string>
        rename(Paths::RemotePath const& source, Paths::RemotePath const& destination, OnEnded onEnded) = 0;

        /**
         * @brief Queues the delete of @p path, a directory only when it is empty.
         */
        virtual std::expected<void, std::string> remove(Paths::RemotePath const& path, OnEnded onEnded) = 0;
    };
}
