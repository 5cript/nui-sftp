#include <backend/file_tracking/failure_reason.hpp>

#include <boost/describe/enum_to_string.hpp>
#include <libssh/sftp.h>

#include <map>
#include <optional>
#include <string>

namespace FileTracking
{
    namespace
    {
        using SharedData::LocalFileProblem;
        using SharedData::OperationCompletionReason;
        using SharedData::OperationErrorType;
        using SharedData::UploadFailureReason;

        /** @brief SSH_FX_NO_SPACE_ON_FILESYSTEM of SFTP version 5, which libssh does not define. */
        constexpr int noSpaceOnFilesystem = 14;
        /** @brief SSH_FX_QUOTA_EXCEEDED of SFTP version 5, which libssh does not define. */
        constexpr int quotaExceeded = 15;

        std::optional<std::string> unlessEmpty(std::string const& text)
        {
            if (text.empty())
                return std::nullopt;
            return text;
        }

        UploadFailureReason localReasonOf(LocalFileProblem problem)
        {
            switch (problem)
            {
                case LocalFileProblem::Locked:
                    return UploadFailureReason::LockedLocally;
                case LocalFileProblem::PermissionDenied:
                    return UploadFailureReason::PermissionDeniedLocally;
                case LocalFileProblem::NotFound:
                case LocalFileProblem::Other:
                    return UploadFailureReason::UnreadableLocally;
                case LocalFileProblem::NoSpace:
                    break;
            }
            return UploadFailureReason::Unknown;
        }

        bool isConnectionLoss(OperationErrorType type)
        {
            return type == OperationErrorType::ChannelClosed || type == OperationErrorType::FutureTimeout;
        }

        /**
         * @brief The reason of an SFTP status code the server answered with, nothing for none.
         */
        std::optional<UploadFailureReason> serverReasonOf(int sftpCode)
        {
            switch (sftpCode)
            {
                case SSH_FX_OK:
                    return std::nullopt;
                case SSH_FX_PERMISSION_DENIED:
                    return UploadFailureReason::PermissionDeniedOnServer;
                case noSpaceOnFilesystem:
                case quotaExceeded:
                    return UploadFailureReason::NoSpaceOnServer;
                case SSH_FX_NO_CONNECTION:
                case SSH_FX_CONNECTION_LOST:
                    return UploadFailureReason::ConnectionProblem;
                default:
                    return UploadFailureReason::ServerRefused;
            }
        }

        /**
         * @brief Why an SFTP call failed that did not get an answer from the server, nothing when it got one.
         */
        std::optional<UploadFailureReason> unansweredReasonOf(SecureShell::SftpError const& error)
        {
            switch (error.wrapperError)
            {
                case SecureShell::WrapperErrors::OwnerNull:
                case SecureShell::WrapperErrors::SharedPtrDestroyed:
                    return UploadFailureReason::ConnectionProblem;
                case SecureShell::WrapperErrors::LocalIoFailure:
                    return UploadFailureReason::UnreadableLocally;
                case SecureShell::WrapperErrors::InvalidPath:
                    return UploadFailureReason::Unknown;
                default:
                    break;
            }
            if (error.sftpError == SSH_FX_OK && error.sshError != 0)
                return UploadFailureReason::ConnectionProblem;
            return std::nullopt;
        }

        /**
         * @brief The text an error has besides its type: what the code that raised it added, else the SFTP message.
         */
        std::optional<std::string> textOf(SharedData::OperationError const& error)
        {
            if (error.extraInfo && !error.extraInfo->empty())
                return error.extraInfo;
            if (error.sftpError)
                return unlessEmpty(error.sftpError->message);
            return std::nullopt;
        }

        std::optional<std::string> serverTextOf(SecureShell::SftpError const& error)
        {
            if (!error.message.empty())
                return error.message;
            return SecureShell::SftpError::sftpErrorToComprehensible(error.sftpError);
        }

        /**
         * @brief Why the operation that ended as @p completed failed, unknown when it did not tell.
         */
        UploadFailureInfo failureOfOperation(SharedData::OperationCompleted const& completed)
        {
            if (!completed.error)
                return {};
            return FileTracking::failureInfoOf(*completed.error);
        }
    }

    UploadFailureInfo failureInfoOf(SharedData::OperationError const& error)
    {
        if (error.localProblem)
            return {.reason = localReasonOf(*error.localProblem), .detail = textOf(error)};
        if (isConnectionLoss(error.type))
            return {.reason = UploadFailureReason::ConnectionProblem, .detail = textOf(error)};
        if (error.sftpError)
        {
            if (const auto unanswered = unansweredReasonOf(*error.sftpError))
                return {.reason = *unanswered, .detail = textOf(error)};
            if (const auto answered = serverReasonOf(error.sftpError->sftpError))
                return {.reason = *answered, .detail = serverTextOf(*error.sftpError)};
        }
        return {.reason = UploadFailureReason::Unknown, .detail = error.toString()};
    }

    UploadFailureInfo failureInfoOf(SharedData::IndexedFailure const& failure)
    {
        if (failure.localProblem)
            return {.reason = localReasonOf(*failure.localProblem)};
        if (isConnectionLoss(failure.type))
            return {.reason = UploadFailureReason::ConnectionProblem};
        if (failure.sftpErrorCode)
        {
            if (const auto answered = serverReasonOf(*failure.sftpErrorCode))
                return {
                    .reason = *answered,
                    .detail = SecureShell::SftpError::sftpErrorToComprehensible(*failure.sftpErrorCode),
                };
        }
        return {
            .reason = UploadFailureReason::Unknown,
            .detail = std::string{boost::describe::enum_to_string(failure.type, "UnknownError")},
        };
    }

    std::vector<FailedUploadEntry>
    failedEntriesOf(SharedData::OperationCompleted const& completed, Paths::LocalPath const& directory)
    {
        std::vector<FailedUploadEntry> failed;
        failed.reserve(completed.failedEntries.size());
        for (auto const& [path, error] : completed.failedEntries)
        {
            if (error.type == OperationErrorType::ChangedSinceCompare)
                continue;
            failed.push_back({
                .path = Paths::LocalPath::fromUtf8(path, directory.platform()).value_or(directory),
                .failure = failureInfoOf(error),
            });
        }
        return failed;
    }

    UploadEnd
    uploadEndOf(SharedData::OperationCompleted const& completed, Paths::LocalPath const& source, bool isDirectory)
    {
        if (completed.error && completed.error->type == OperationErrorType::ChangedSinceCompare)
            return {.reason = completed.reason, .changedOnServer = true};
        switch (completed.reason)
        {
            case OperationCompletionReason::Failed:
                return {.reason = OperationCompletionReason::Failed, .failure = failureOfOperation(completed)};
            case OperationCompletionReason::Completed:
                if (isDirectory)
                    return {
                        .reason = OperationCompletionReason::Completed,
                        .failedEntries = failedEntriesOf(completed, source),
                    };
                break;
            case OperationCompletionReason::Canceled:
                break;
        }
        return {.reason = completed.reason};
    }

    std::vector<UploadEnd>
    entryEndsOf(SharedData::OperationCompleted const& completed, std::vector<Paths::LocalPath> const& sources)
    {
        if (completed.reason != OperationCompletionReason::Completed)
        {
            UploadEnd ended{.reason = completed.reason};
            if (completed.reason == OperationCompletionReason::Failed)
                ended.failure = failureOfOperation(completed);
            return std::vector<UploadEnd>(sources.size(), ended);
        }
        std::map<std::size_t, SharedData::IndexedFailure> skipped;
        for (auto const& failure : completed.indexedFailures)
            skipped.emplace(static_cast<std::size_t>(failure.index), failure);
        std::map<std::string, SharedData::OperationError> failedBySource;
        for (auto const& [path, error] : completed.failedEntries)
            failedBySource.emplace(path, error);

        const auto endOf = [&skipped, &failedBySource, &sources](std::size_t index) -> UploadEnd
        {
            const auto found = skipped.find(index);
            if (found == skipped.end())
                return {.reason = OperationCompletionReason::Completed};
            auto const& failure = found->second;
            if (failure.type == OperationErrorType::ChangedSinceCompare)
                return {.reason = OperationCompletionReason::Failed, .changedOnServer = true};
            const auto failed = failedBySource.find(sources[index].toUtf8());
            if (failed == failedBySource.end())
            {
                // Left out without an error: its source was gone.
                if (failure.type == OperationErrorType::FileNotFound)
                    return {.reason = OperationCompletionReason::Canceled};
                return {.reason = OperationCompletionReason::Failed, .failure = failureInfoOf(failure)};
            }
            return {.reason = OperationCompletionReason::Failed, .failure = failureInfoOf(failed->second)};
        };
        std::vector<UploadEnd> ends;
        ends.reserve(sources.size());
        for (std::size_t index = 0; index < sources.size(); ++index)
            ends.push_back(endOf(index));
        return ends;
    }

    bool asksDestinationState(UploadEnd const& ended, bool isDirectory)
    {
        if (ended.changedOnServer)
            return true;
        return ended.reason == OperationCompletionReason::Completed && !isDirectory;
    }
}
