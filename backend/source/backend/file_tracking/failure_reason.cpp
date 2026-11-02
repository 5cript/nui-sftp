#include <backend/file_tracking/failure_reason.hpp>
#include <backend/sftp/local_file_problem.hpp>
#include <utility/algorithm/case_convert.hpp>
#include <utility/error_message.hpp>

#include <boost/describe/enum_to_string.hpp>

#include <map>
#include <optional>
#include <string>

namespace FileTracking
{
    namespace
    {
        using RemoteIo::RemoteErrorKind;
        using RemoteIo::WrapperErrors;
        using SharedData::LocalFileProblem;
        using SharedData::OperationCompletionReason;
        using SharedData::OperationErrorType;
        using SharedData::UploadFailureReason;

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
         * @brief The reason of what went wrong on the remote side, nothing when it does not tell: a cancelled or
         *        truncated transfer is no answer of the server.
         */
        std::optional<UploadFailureReason> remoteReasonOf(RemoteErrorKind kind)
        {
            switch (kind)
            {
                case RemoteErrorKind::None:
                case RemoteErrorKind::Cancelled:
                case RemoteErrorKind::Truncated:
                    return std::nullopt;
                case RemoteErrorKind::PermissionDenied:
                    return UploadFailureReason::PermissionDeniedOnServer;
                case RemoteErrorKind::NoSpace:
                case RemoteErrorKind::QuotaExceeded:
                    return UploadFailureReason::NoSpaceOnServer;
                case RemoteErrorKind::NoConnection:
                case RemoteErrorKind::ConnectionLost:
                case RemoteErrorKind::ConnectionLimit:
                case RemoteErrorKind::Busy:
                case RemoteErrorKind::TimedOut:
                case RemoteErrorKind::TrustRequired:
                case RemoteErrorKind::AuthenticationFailed:
                    return UploadFailureReason::ConnectionProblem;
                case RemoteErrorKind::NoSuchFile:
                case RemoteErrorKind::AlreadyExists:
                case RemoteErrorKind::Unsupported:
                case RemoteErrorKind::EndOfFile:
                case RemoteErrorKind::ProtocolViolation:
                case RemoteErrorKind::Failure:
                    return UploadFailureReason::ServerRefused;
            }
            return std::nullopt;
        }

        /**
         * @brief Why a remote call failed that never reached the server, nothing when it did.
         */
        std::optional<UploadFailureReason> unsentReasonOf(WrapperErrors wrapperError)
        {
            switch (wrapperError)
            {
                case WrapperErrors::OwnerNull:
                case WrapperErrors::SharedPtrDestroyed:
                    return UploadFailureReason::ConnectionProblem;
                case WrapperErrors::LocalIoFailure:
                    return UploadFailureReason::UnreadableLocally;
                case WrapperErrors::InvalidPath:
                    return UploadFailureReason::Unknown;
                default:
                    return std::nullopt;
            }
        }

        /**
         * @brief The kind as a sentence, "No such file".
         */
        std::string kindInWords(RemoteErrorKind kind)
        {
            auto words = RemoteIo::toString(kind);
            if (!words.empty())
                words.front() = Utility::Algorithm::asciiToUpper(words.front());
            return words;
        }

        /**
         * @brief The text an error has besides its type: what the code that raised it added, else the remote message.
         */
        std::optional<std::string> textOf(SharedData::OperationError const& error)
        {
            if (error.extraInfo && !error.extraInfo->empty())
                return error.extraInfo;
            if (error.remoteError)
                return unlessEmpty(error.remoteError->message);
            return std::nullopt;
        }

        std::optional<std::string> remoteTextOf(RemoteIo::RemoteError const& error)
        {
            if (!error.message.empty())
                return error.message;
            return kindInWords(error.kind);
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
        if (error.remoteError)
        {
            if (const auto unsent = unsentReasonOf(error.remoteError->wrapperError))
                return {.reason = *unsent, .detail = textOf(error)};
            if (const auto remote = remoteReasonOf(error.remoteError->kind))
                return {.reason = *remote, .detail = remoteTextOf(*error.remoteError)};
        }
        return {.reason = UploadFailureReason::Unknown, .detail = error.toString()};
    }

    UploadFailureInfo failureInfoOf(SharedData::IndexedFailure const& failure)
    {
        if (failure.localProblem)
            return {.reason = localReasonOf(*failure.localProblem)};
        if (isConnectionLoss(failure.type))
            return {.reason = UploadFailureReason::ConnectionProblem};
        if (failure.remoteErrorKind)
        {
            if (const auto remote = remoteReasonOf(*failure.remoteErrorKind))
                return {.reason = *remote, .detail = kindInWords(*failure.remoteErrorKind)};
        }
        return {
            .reason = UploadFailureReason::Unknown,
            .detail = std::string{boost::describe::enum_to_string(failure.type, "UnknownError")},
        };
    }

    UploadFailureInfo failureInfoOf(std::error_code const& error)
    {
        return {
            .reason = localReasonOf(localFileProblemOf(error)), .detail = unlessEmpty(Utility::errorMessage(error))
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
