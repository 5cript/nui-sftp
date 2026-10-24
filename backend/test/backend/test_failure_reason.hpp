#pragma once

#include <backend/file_tracking/failure_reason.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Test::FailureReason
{
    using namespace FileTracking;
    using SharedData::LocalFileProblem;
    using SharedData::OperationCompletionReason;
    using SharedData::OperationErrorType;
    using SharedData::UploadFailureReason;

    namespace FailureReasonTestDetail
    {
        inline Paths::LocalPath local(std::string_view path)
        {
            return Paths::LocalPath::fromUtf8(path, Paths::LocalPlatform::Posix).value();
        }

        inline SharedData::OperationError sftpFailure(int code, std::string message = {})
        {
            return {
                .type = OperationErrorType::SftpError,
                .sftpError = SecureShell::SftpError{.message = std::move(message), .sftpError = code},
            };
        }

        inline SharedData::OperationCompleted
        completedAs(OperationCompletionReason reason, std::optional<SharedData::OperationError> error = std::nullopt)
        {
            return {
                .reason = reason,
                .operationId = Ids::generateOperationId(),
                .completionTime = std::chrono::system_clock::now(),
                .error = std::move(error),
            };
        }
    }

    using namespace FailureReasonTestDetail;

    TEST(FailureReasonTests, ALockedSourceIsLockedLocallyWithTheSystemsMessage)
    {
        const auto failure = failureInfoOf(
            SharedData::OperationError{
                .type = OperationErrorType::OpenFailure,
                .extraInfo = "The process cannot access the file",
                .localProblem = LocalFileProblem::Locked,
            }
        );
        EXPECT_EQ(failure.reason, UploadFailureReason::LockedLocally);
        EXPECT_EQ(failure.detail, "The process cannot access the file");
    }

    TEST(FailureReasonTests, LocalProblemsMapToTheirLocalReasons)
    {
        const auto reasonOf = [](LocalFileProblem problem)
        {
            return failureInfoOf(
                SharedData::OperationError{.type = OperationErrorType::OpenFailure, .localProblem = problem}
            )
                .reason;
        };
        EXPECT_EQ(reasonOf(LocalFileProblem::Locked), UploadFailureReason::LockedLocally);
        EXPECT_EQ(reasonOf(LocalFileProblem::PermissionDenied), UploadFailureReason::PermissionDeniedLocally);
        EXPECT_EQ(reasonOf(LocalFileProblem::NotFound), UploadFailureReason::UnreadableLocally);
        EXPECT_EQ(reasonOf(LocalFileProblem::Other), UploadFailureReason::UnreadableLocally);
        EXPECT_EQ(reasonOf(LocalFileProblem::NoSpace), UploadFailureReason::Unknown);
    }

    TEST(FailureReasonTests, ALocalProblemComesBeforeTheServersAnswer)
    {
        auto error = sftpFailure(3);
        error.localProblem = LocalFileProblem::PermissionDenied;
        EXPECT_EQ(failureInfoOf(error).reason, UploadFailureReason::PermissionDeniedLocally);
    }

    TEST(FailureReasonTests, PermissionDeniedByTheServerIsCodeThree)
    {
        const auto failure = failureInfoOf(sftpFailure(3));
        EXPECT_EQ(failure.reason, UploadFailureReason::PermissionDeniedOnServer);
    }

    TEST(FailureReasonTests, NoSpaceAndAnExceededQuotaAreNoSpaceOnTheServer)
    {
        EXPECT_EQ(failureInfoOf(sftpFailure(14)).reason, UploadFailureReason::NoSpaceOnServer);
        EXPECT_EQ(failureInfoOf(sftpFailure(15)).reason, UploadFailureReason::NoSpaceOnServer);
    }

    TEST(FailureReasonTests, OtherCodesAreRefusedByTheServerWithItsMessage)
    {
        const auto failure = failureInfoOf(sftpFailure(4, "Disk quota exceeded"));
        EXPECT_EQ(failure.reason, UploadFailureReason::ServerRefused);
        EXPECT_EQ(failure.detail, "Disk quota exceeded");

        const auto noSuchFile = failureInfoOf(sftpFailure(2));
        EXPECT_EQ(noSuchFile.reason, UploadFailureReason::ServerRefused);
        EXPECT_EQ(noSuchFile.detail, "No such file");
    }

    TEST(FailureReasonTests, AClosedChannelATimeoutAndALostConnectionAreConnectionProblems)
    {
        const auto closed = failureInfoOf(
            SharedData::OperationError{
                .type = OperationErrorType::ChannelClosed,
                .extraInfo = "The SFTP channel was closed",
            }
        );
        EXPECT_EQ(closed.reason, UploadFailureReason::ConnectionProblem);
        EXPECT_EQ(closed.detail, "The SFTP channel was closed");
        EXPECT_EQ(
            failureInfoOf(SharedData::OperationError{.type = OperationErrorType::FutureTimeout}).reason,
            UploadFailureReason::ConnectionProblem
        );
        EXPECT_EQ(failureInfoOf(sftpFailure(6)).reason, UploadFailureReason::ConnectionProblem);
        EXPECT_EQ(failureInfoOf(sftpFailure(7)).reason, UploadFailureReason::ConnectionProblem);
    }

    TEST(FailureReasonTests, AGoneSessionOrAnSshErrorIsAConnectionProblem)
    {
        const auto wrapped = [](SecureShell::WrapperErrors wrapperError)
        {
            return failureInfoOf(
                SharedData::OperationError{
                    .type = OperationErrorType::SftpError,
                    .sftpError = SecureShell::SftpError{.message = "gone", .wrapperError = wrapperError},
                }
            )
                .reason;
        };
        EXPECT_EQ(wrapped(SecureShell::WrapperErrors::SharedPtrDestroyed), UploadFailureReason::ConnectionProblem);
        EXPECT_EQ(wrapped(SecureShell::WrapperErrors::OwnerNull), UploadFailureReason::ConnectionProblem);
        EXPECT_EQ(wrapped(SecureShell::WrapperErrors::LocalIoFailure), UploadFailureReason::UnreadableLocally);
        EXPECT_EQ(
            failureInfoOf(
                SharedData::OperationError{
                    .type = OperationErrorType::SftpError,
                    .sftpError = SecureShell::SftpError{.message = "Socket error", .sshError = -1},
                }
            )
                .reason,
            UploadFailureReason::ConnectionProblem
        );
    }

    TEST(FailureReasonTests, AnythingElseIsUnknownWithWhatTheErrorTells)
    {
        const SharedData::OperationError error{
            .type = OperationErrorType::OperationNotPossibleOnFileType,
            .extraInfo = "A socket",
        };
        const auto failure = failureInfoOf(error);
        EXPECT_EQ(failure.reason, UploadFailureReason::Unknown);
        ASSERT_TRUE(failure.detail.has_value());
        EXPECT_EQ(*failure.detail, error.toString());
        EXPECT_NE(failure.detail->find("A socket"), std::string::npos);
    }

    TEST(FailureReasonTests, AnIndexedFailureMapsLikeTheErrorItStandsFor)
    {
        const auto indexed = [](SharedData::IndexedFailure failure)
        {
            return failureInfoOf(failure);
        };
        EXPECT_EQ(
            indexed({.type = OperationErrorType::OpenFailure, .localProblem = LocalFileProblem::Locked}).reason,
            UploadFailureReason::LockedLocally
        );
        EXPECT_EQ(
            indexed({.type = OperationErrorType::OpenFailure, .localProblem = LocalFileProblem::PermissionDenied})
                .reason,
            UploadFailureReason::PermissionDeniedLocally
        );
        EXPECT_EQ(
            indexed({.type = OperationErrorType::SourceFileNotGood, .localProblem = LocalFileProblem::Other}).reason,
            UploadFailureReason::UnreadableLocally
        );
        EXPECT_EQ(
            indexed({.type = OperationErrorType::SftpError, .sftpErrorCode = 3}).reason,
            UploadFailureReason::PermissionDeniedOnServer
        );
        EXPECT_EQ(
            indexed({.type = OperationErrorType::SftpError, .sftpErrorCode = 14}).reason,
            UploadFailureReason::NoSpaceOnServer
        );
        EXPECT_EQ(
            indexed({.type = OperationErrorType::SftpError, .sftpErrorCode = 15}).reason,
            UploadFailureReason::NoSpaceOnServer
        );
        EXPECT_EQ(
            indexed({.type = OperationErrorType::SftpError, .sftpErrorCode = 7}).reason,
            UploadFailureReason::ConnectionProblem
        );
        EXPECT_EQ(indexed({.type = OperationErrorType::ChannelClosed}).reason, UploadFailureReason::ConnectionProblem);
        EXPECT_EQ(indexed({.type = OperationErrorType::FutureTimeout}).reason, UploadFailureReason::ConnectionProblem);

        const auto refused = indexed({.type = OperationErrorType::SftpError, .sftpErrorCode = 4});
        EXPECT_EQ(refused.reason, UploadFailureReason::ServerRefused);
        EXPECT_EQ(refused.detail, "Failure");

        const auto unknown = indexed({.type = OperationErrorType::SizeMismatch});
        EXPECT_EQ(unknown.reason, UploadFailureReason::Unknown);
        EXPECT_EQ(unknown.detail, "SizeMismatch");
    }

    TEST(FailureReasonTests, AFailedUploadTellsWhyAndOneWithoutAnErrorIsUnknown)
    {
        const auto failed = uploadEndOf(
            completedAs(OperationCompletionReason::Failed, sftpFailure(3)), local("/home/user/a.txt"), false
        );
        EXPECT_EQ(failed.reason, OperationCompletionReason::Failed);
        EXPECT_FALSE(failed.changedOnServer);
        ASSERT_TRUE(failed.failure.has_value());
        EXPECT_EQ(failed.failure->reason, UploadFailureReason::PermissionDeniedOnServer);

        const auto silent =
            uploadEndOf(completedAs(OperationCompletionReason::Failed), local("/home/user/a.txt"), false);
        ASSERT_TRUE(silent.failure.has_value());
        EXPECT_EQ(silent.failure->reason, UploadFailureReason::Unknown);
    }

    TEST(FailureReasonTests, ADestinationChangedOnTheServerIsNoFailureAndAsksWhatIsThere)
    {
        const auto changed = uploadEndOf(
            completedAs(
                OperationCompletionReason::Canceled,
                SharedData::OperationError{.type = OperationErrorType::ChangedSinceCompare}
            ),
            local("/home/user/a.txt"),
            false
        );
        EXPECT_EQ(changed.reason, OperationCompletionReason::Canceled);
        EXPECT_TRUE(changed.changedOnServer);
        EXPECT_FALSE(changed.failure.has_value());
        EXPECT_TRUE(asksDestinationState(changed, false));
    }

    TEST(FailureReasonTests, CompletedAndCancelledUploadsTellNoFailure)
    {
        const auto completed =
            uploadEndOf(completedAs(OperationCompletionReason::Completed), local("/home/user/a.txt"), false);
        EXPECT_EQ(completed.reason, OperationCompletionReason::Completed);
        EXPECT_FALSE(completed.failure.has_value());
        EXPECT_TRUE(asksDestinationState(completed, false));

        const auto cancelled =
            uploadEndOf(completedAs(OperationCompletionReason::Canceled), local("/home/user/a.txt"), false);
        EXPECT_EQ(cancelled.reason, OperationCompletionReason::Canceled);
        EXPECT_FALSE(cancelled.changedOnServer);
        EXPECT_FALSE(cancelled.failure.has_value());
        EXPECT_FALSE(asksDestinationState(cancelled, false));
    }

    TEST(FailureReasonTests, ACompletedDirectoryUploadTellsTheEntriesItLeftOut)
    {
        auto completed = completedAs(OperationCompletionReason::Completed);
        completed.failedEntries = {
            {"/home/user/project/locked.txt",
                SharedData::OperationError{
                    .type = OperationErrorType::OpenFailure, .localProblem = LocalFileProblem::Locked
                }},
            {"/home/user/project/refused.txt", sftpFailure(3)},
            {"/home/user/project/changed.txt",
                SharedData::OperationError{.type = OperationErrorType::ChangedSinceCompare}},
            {"project/entry", SharedData::OperationError{.type = OperationErrorType::InvalidName}},
        };
        const auto directory = local("/home/user/project");

        const auto ended = uploadEndOf(completed, directory, true);
        EXPECT_EQ(ended.reason, OperationCompletionReason::Completed);
        EXPECT_FALSE(ended.failure.has_value());
        EXPECT_FALSE(asksDestinationState(ended, true));
        ASSERT_EQ(ended.failedEntries.size(), 3u);
        EXPECT_EQ(ended.failedEntries[0].path, local("/home/user/project/locked.txt"));
        EXPECT_EQ(ended.failedEntries[0].failure.reason, UploadFailureReason::LockedLocally);
        EXPECT_EQ(ended.failedEntries[1].path, local("/home/user/project/refused.txt"));
        EXPECT_EQ(ended.failedEntries[1].failure.reason, UploadFailureReason::PermissionDeniedOnServer);
        // Its name is no path, it is told as the directory.
        EXPECT_EQ(ended.failedEntries[2].path, directory);
        EXPECT_EQ(ended.failedEntries[2].failure.reason, UploadFailureReason::Unknown);

        EXPECT_TRUE(uploadEndOf(completed, local("/home/user/project"), false).failedEntries.empty());
    }

    TEST(FailureReasonTests, EachEntryOfACompletedBulkEndsByItsIndex)
    {
        const std::vector<Paths::LocalPath> sources{
            local("/home/user/sent.txt"),
            local("/home/user/changed.txt"),
            local("/home/user/gone.txt"),
            local("/home/user/refused.txt"),
            local("/home/user/locked.txt"),
        };
        auto completed = completedAs(OperationCompletionReason::Completed);
        completed.indexedFailures = {
            {.index = 4u, .type = OperationErrorType::OpenFailure, .localProblem = LocalFileProblem::Locked},
            {.index = 1u, .type = OperationErrorType::ChangedSinceCompare},
            {.index = 2u, .type = OperationErrorType::FileNotFound},
            {.index = 3u, .type = OperationErrorType::SftpError, .sftpErrorCode = 4},
        };
        completed.failedEntries = {
            {"/home/user/refused.txt", sftpFailure(4, "Read-only file system")},
            {"/home/user/locked.txt",
                SharedData::OperationError{
                    .type = OperationErrorType::OpenFailure,
                    .extraInfo = "Locked by another process",
                    .localProblem = LocalFileProblem::Locked,
                }},
        };

        const auto ends = entryEndsOf(completed, sources);
        ASSERT_EQ(ends.size(), 5u);
        EXPECT_EQ(ends[0].reason, OperationCompletionReason::Completed);
        EXPECT_FALSE(ends[0].failure.has_value());
        EXPECT_TRUE(asksDestinationState(ends[0], false));

        EXPECT_EQ(ends[1].reason, OperationCompletionReason::Failed);
        EXPECT_TRUE(ends[1].changedOnServer);
        EXPECT_FALSE(ends[1].failure.has_value());
        EXPECT_TRUE(asksDestinationState(ends[1], false));

        // Its source was gone, the deletion is mirrored apart.
        EXPECT_EQ(ends[2].reason, OperationCompletionReason::Canceled);
        EXPECT_FALSE(ends[2].failure.has_value());
        EXPECT_FALSE(asksDestinationState(ends[2], false));

        EXPECT_EQ(ends[3].reason, OperationCompletionReason::Failed);
        ASSERT_TRUE(ends[3].failure.has_value());
        EXPECT_EQ(ends[3].failure->reason, UploadFailureReason::ServerRefused);
        EXPECT_EQ(ends[3].failure->detail, "Read-only file system");
        EXPECT_FALSE(asksDestinationState(ends[3], false));

        ASSERT_TRUE(ends[4].failure.has_value());
        EXPECT_EQ(ends[4].failure->reason, UploadFailureReason::LockedLocally);
        EXPECT_EQ(ends[4].failure->detail, "Locked by another process");
    }

    TEST(FailureReasonTests, AnEntryLeftOutWithoutAnErrorOfItsOwnTellsItsIndexedFailure)
    {
        auto completed = completedAs(OperationCompletionReason::Completed);
        completed.indexedFailures = {{.index = 0u, .type = OperationErrorType::SftpError, .sftpErrorCode = 3}};

        const auto ends = entryEndsOf(completed, {local("/home/user/a.txt")});
        ASSERT_EQ(ends.size(), 1u);
        EXPECT_EQ(ends[0].reason, OperationCompletionReason::Failed);
        ASSERT_TRUE(ends[0].failure.has_value());
        EXPECT_EQ(ends[0].failure->reason, UploadFailureReason::PermissionDeniedOnServer);
    }

    TEST(FailureReasonTests, EveryEntryOfAFailedOrCancelledBulkEndsLikeTheBulk)
    {
        const std::vector<Paths::LocalPath> sources{local("/home/user/a.txt"), local("/home/user/b.txt")};
        const auto failed = entryEndsOf(
            completedAs(
                OperationCompletionReason::Failed, SharedData::OperationError{.type = OperationErrorType::ChannelClosed}
            ),
            sources
        );
        ASSERT_EQ(failed.size(), 2u);
        for (auto const& ended : failed)
        {
            EXPECT_EQ(ended.reason, OperationCompletionReason::Failed);
            ASSERT_TRUE(ended.failure.has_value());
            EXPECT_EQ(ended.failure->reason, UploadFailureReason::ConnectionProblem);
        }

        const auto cancelled = entryEndsOf(completedAs(OperationCompletionReason::Canceled), sources);
        ASSERT_EQ(cancelled.size(), 2u);
        for (auto const& ended : cancelled)
        {
            EXPECT_EQ(ended.reason, OperationCompletionReason::Canceled);
            EXPECT_FALSE(ended.failure.has_value());
        }
    }
}
