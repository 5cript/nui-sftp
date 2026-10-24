#pragma once

#include "test_file_tracking_texts.hpp"

#include <frontend/session_components/file_tracking/tracking_model.hpp>
#include <paths/paths.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace FileTracking::Test
{
    namespace TrackingModelTestDetail
    {
        using FileTrackingTextsTestDetail::localPath;
        using FileTrackingTextsTestDetail::sampleTexts;
        using Names = std::vector<std::string>;

        inline Paths::RemotePath remotePath(std::string_view path)
        {
            return *Paths::RemotePath::make(path);
        }

        inline Ids::WatchId watchIdOf(std::string const& watchId)
        {
            return Ids::makeWatchId(watchId);
        }

        inline SharedData::TrackedEntry watchEntry(std::string const& watchId, std::string const& name)
        {
            SharedData::TrackedEntry entry{};
            entry.watchId = Ids::makeWatchId(watchId);
            entry.name = name;
            entry.remotePath = remotePath(fmt::format("/srv/{}", name));
            entry.localPath = localPath(fmt::format("/home/user/{}", name));
            return entry;
        }

        inline SharedData::TrackedEntry
        stoppedEntry(std::string const& watchId, std::string const& name, SharedData::WatchStopReason reason)
        {
            auto entry = watchEntry(watchId, name);
            entry.stop = SharedData::WatchStop{.reason = reason, .stoppedAt = 1'700'000'000u};
            return entry;
        }

        inline SharedData::TrackedEntry
        copyEntry(std::string const& watchId, std::string const& name, std::string const& instanceId)
        {
            auto entry = watchEntry(watchId, name);
            entry.downloadedCopy = true;
            entry.instanceId = Ids::makeInstanceId(instanceId);
            return entry;
        }

        inline SharedData::WatchConflict
        conflictOf(std::string const& conflictId, std::string const& watchId, std::uint64_t raisedAt)
        {
            SharedData::WatchConflict conflict{};
            conflict.conflictId = Ids::makeConflictId(conflictId);
            conflict.watchId = Ids::makeWatchId(watchId);
            conflict.localPath = localPath("/home/user/notes.txt");
            conflict.remotePath = remotePath("/srv/notes.txt");
            conflict.raisedAt = raisedAt;
            return conflict;
        }

        inline SharedData::UploadFailure
        failureOf(std::string const& failureId, std::string const& watchId, std::uint64_t failedAt)
        {
            SharedData::UploadFailure failure{};
            failure.failureId = Ids::makeFailureId(failureId);
            failure.watchId = Ids::makeWatchId(watchId);
            failure.localPath = localPath("/home/user/notes.txt");
            failure.remotePath = remotePath("/srv/notes.txt");
            failure.failedAt = failedAt;
            return failure;
        }

        inline SharedData::WatchProgress progressOf(
            std::string const& watchId,
            std::uint64_t changesPending,
            std::uint64_t changesDone,
            std::uint64_t bytesDone,
            std::uint64_t bytesTotal
        )
        {
            SharedData::WatchProgress progress{};
            progress.watchId = Ids::makeWatchId(watchId);
            progress.changesPending = changesPending;
            progress.changesDone = changesDone;
            progress.bytesDone = bytesDone;
            progress.bytesTotal = bytesTotal;
            return progress;
        }

        inline SharedData::OrphanedCopy orphanOf(std::string const& instanceId, std::string const& name)
        {
            SharedData::OrphanedCopy copy{};
            copy.instanceId = Ids::makeInstanceId(instanceId);
            copy.name = name;
            copy.directory = localPath(fmt::format("/home/user/.cache/nui-sftp/{}", instanceId));
            copy.copyPath = localPath(fmt::format("/home/user/.cache/nui-sftp/{}/{}", instanceId, name));
            return copy;
        }

        inline SharedData::OrphanedCopies orphansOf(std::vector<SharedData::OrphanedCopy> copies)
        {
            return SharedData::OrphanedCopies{.copies = std::move(copies)};
        }

        /**
         * @brief "watch:<id>" or "instance:<id>" per member, readable in failure messages.
         */
        inline Names namesOf(std::vector<Selection> const& members)
        {
            Names names;
            for (auto const& member : members)
            {
                if (auto const* watchId = std::get_if<Ids::WatchId>(&member))
                    names.push_back(fmt::format("watch:{}", watchId->value()));
                else if (auto const* instanceId = std::get_if<Ids::InstanceId>(&member))
                    names.push_back(fmt::format("instance:{}", instanceId->value()));
                else
                    names.push_back("none");
            }
            return names;
        }

        inline Names conflictNamesOf(std::vector<SharedData::WatchConflict> const& conflicts)
        {
            Names names;
            for (auto const& conflict : conflicts)
                names.push_back(conflict.conflictId.value());
            return names;
        }

        inline Names failureNamesOf(std::vector<SharedData::UploadFailure> const& failures)
        {
            Names names;
            for (auto const& failure : failures)
                names.push_back(failure.failureId.value());
            return names;
        }

        inline Names membersOf(TrackingModel const& model, OutlinerGroup group, std::string_view filter = {})
        {
            return namesOf(model.members(group, filter));
        }

        inline std::vector<SharedData::WatchConflict>
        allConflictsOf(TrackingModel const& model, std::string const& watchId)
        {
            return model.conflictsOf(watchIdOf(watchId), 0u, model.conflictCountOf(watchIdOf(watchId)));
        }

        inline std::vector<SharedData::UploadFailure>
        allFailuresOf(TrackingModel const& model, std::string const& watchId)
        {
            return model.failuresOf(watchIdOf(watchId), 0u, model.failureCountOf(watchIdOf(watchId)));
        }

        inline std::vector<Ids::ConflictId> conflictIdsOf(Names const& names)
        {
            std::vector<Ids::ConflictId> conflictIds{};
            for (auto const& name : names)
                conflictIds.push_back(Ids::makeConflictId(name));
            return conflictIds;
        }

        inline std::vector<Ids::FailureId> failureIdsOf(Names const& names)
        {
            std::vector<Ids::FailureId> failureIds{};
            for (auto const& name : names)
                failureIds.push_back(Ids::makeFailureId(name));
            return failureIds;
        }

        /**
         * @brief Shows @p count conflicts "<prefix>-0" to "<prefix>-<count - 1>" of @p watchId, in that order.
         */
        inline void
        showConflicts(TrackingModel& model, std::string const& prefix, std::string const& watchId, int count)
        {
            for (auto index = 0; index < count; ++index)
                model.showConflict(conflictOf(fmt::format("{}-{}", prefix, index), watchId, 100u));
        }

        /**
         * @brief Shows @p count failures "<prefix>-0" to "<prefix>-<count - 1>" of @p watchId, in that order.
         */
        inline void showFailures(TrackingModel& model, std::string const& prefix, std::string const& watchId, int count)
        {
            for (auto index = 0; index < count; ++index)
                model.showFailure(failureOf(fmt::format("{}-{}", prefix, index), watchId, 100u));
        }
    }

    TEST(TrackingModelTests, AnUnknownWatchIsWatchingWithoutText)
    {
        using namespace TrackingModelTestDetail;
        const TrackingModel model;
        const auto texts = sampleTexts();

        EXPECT_EQ(model.entryOf(watchIdOf("unknown")), nullptr);
        EXPECT_EQ(model.progressOf(watchIdOf("unknown")), nullptr);
        EXPECT_EQ(model.statusOf(watchIdOf("unknown")), WatchStatus::Watching);
        EXPECT_EQ(model.groupOf(watchIdOf("unknown")), OutlinerGroup::Watching);
        EXPECT_EQ(model.sideTextOf(watchIdOf("unknown"), texts), "");
        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("unknown")), 0.0);
    }

    TEST(TrackingModelTests, AnIdleWatchIsWatchingAndUpToDate)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        model.showEntry(watchEntry("watch", "notes.txt"));

        ASSERT_NE(model.entryOf(watchIdOf("watch")), nullptr);
        EXPECT_EQ(model.entryOf(watchIdOf("watch"))->name, "notes.txt");
        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Watching);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::Watching);
        EXPECT_EQ(model.statusTextOf(watchIdOf("watch"), texts), "Up to date");
        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "");
    }

    TEST(TrackingModelTests, AnUploadingEntryIsSyncing)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        auto entry = watchEntry("watch", "notes.txt");
        entry.uploading = true;
        model.showEntry(entry);

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Syncing);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::Syncing);
        EXPECT_EQ(model.statusTextOf(watchIdOf("watch"), texts), "Syncing");
        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "");
    }

    TEST(TrackingModelTests, PendingChangesAreSyncingUntilTheyEnd)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showWatchProgress(progressOf("watch", 2u, 1u, 0u, 0u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Syncing);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::Syncing);

        model.showWatchProgress(progressOf("watch", 0u, 3u, 0u, 0u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Watching);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::Watching);
    }

    TEST(TrackingModelTests, ConflictsNeedYouAndAreCounted)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showConflict(conflictOf("conflict-1", "watch", 100u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Conflicts);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::NeedsYou);
        EXPECT_EQ(model.statusTextOf(watchIdOf("watch"), texts), "1 conflict");
        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "1 conflict");

        model.showConflict(conflictOf("conflict-2", "watch", 200u));

        EXPECT_EQ(model.statusTextOf(watchIdOf("watch"), texts), "2 conflicts");
        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "2 conflicts");
    }

    TEST(TrackingModelTests, FailuresNeedYouAndAreCounted)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showFailure(failureOf("failure-1", "watch", 100u));
        model.showFailure(failureOf("failure-2", "watch", 200u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Failed);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::NeedsYou);
        EXPECT_EQ(model.statusTextOf(watchIdOf("watch"), texts), "2 failed");
        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "2 failed");
    }

    TEST(TrackingModelTests, ConflictsWinOverFailures)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showFailure(failureOf("failure", "watch", 100u));
        model.showConflict(conflictOf("conflict", "watch", 100u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Conflicts);
        EXPECT_EQ(model.statusTextOf(watchIdOf("watch"), texts), "1 conflict");
        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "1 conflict");
    }

    TEST(TrackingModelTests, NeedingYouWinsOverSyncing)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        auto entry = watchEntry("watch", "project");
        entry.uploading = true;
        model.showEntry(entry);
        model.showWatchProgress(progressOf("watch", 4u, 0u, 10u, 100u));
        model.showFailure(failureOf("failure", "watch", 100u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Failed);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::NeedsYou);

        model.showConflict(conflictOf("conflict", "watch", 100u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Conflicts);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::NeedsYou);
    }

    TEST(TrackingModelTests, StoppedWinsOverEverythingElse)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        auto entry = stoppedEntry("watch", "project", SharedData::WatchStopReason::StoppedByUser);
        entry.uploading = true;
        model.showEntry(entry);
        model.showWatchProgress(progressOf("watch", 4u, 0u, 10u, 100u));
        model.showFailure(failureOf("failure", "watch", 100u));
        model.showConflict(conflictOf("conflict", "watch", 100u));

        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Stopped);
        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::Stopped);
        EXPECT_EQ(model.statusTextOf(watchIdOf("watch"), texts), "Stopped");
    }

    TEST(TrackingModelTests, TheSideTextPrefersConflictsThenFailuresThenTheStopReason)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        model.showEntry(stoppedEntry("watch", "project", SharedData::WatchStopReason::Overlap));
        model.showConflict(conflictOf("conflict", "watch", 100u));
        model.showFailure(failureOf("failure", "watch", 100u));

        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "1 conflict");

        model.removeConflicts({Ids::makeConflictId("conflict")});

        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "1 failed");

        model.removeFailures({Ids::makeFailureId("failure")});

        EXPECT_EQ(model.sideTextOf(watchIdOf("watch"), texts), "overlap");
    }

    TEST(TrackingModelTests, AStoppedWatchShowsItsReasonBesideIt)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        const auto texts = sampleTexts();
        model.showEntry(stoppedEntry("user", "user.txt", SharedData::WatchStopReason::StoppedByUser));
        model.showEntry(stoppedEntry("overlap", "overlap.txt", SharedData::WatchStopReason::Overlap));
        model.showEntry(stoppedEntry("gone", "gone.txt", SharedData::WatchStopReason::CopyGone));
        model.showEntry(stoppedEntry("lost", "lost.txt", SharedData::WatchStopReason::ConnectionLost));

        EXPECT_EQ(model.sideTextOf(watchIdOf("user"), texts), "stopped");
        EXPECT_EQ(model.sideTextOf(watchIdOf("overlap"), texts), "overlap");
        EXPECT_EQ(model.sideTextOf(watchIdOf("gone"), texts), "deleted");
        EXPECT_EQ(model.sideTextOf(watchIdOf("lost"), texts), "connection lost");
        EXPECT_EQ(
            membersOf(model, OutlinerGroup::Stopped), (Names{"watch:gone", "watch:lost", "watch:overlap", "watch:user"})
        );
    }

    TEST(TrackingModelTests, AnsweredConflictsAndHandledFailuresReturnTheWatchToItsGroup)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showConflict(conflictOf("conflict", "watch", 100u));
        model.showFailure(failureOf("failure", "watch", 100u));
        model.removeConflicts({Ids::makeConflictId("conflict")});

        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::NeedsYou);
        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Failed);

        model.removeFailures({Ids::makeFailureId("failure")});

        EXPECT_EQ(model.groupOf(watchIdOf("watch")), OutlinerGroup::Watching);
        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Watching);
    }

    TEST(TrackingModelTests, ShowingAnEntryAgainReplacesIt)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showEntry(stoppedEntry("watch", "renamed.txt", SharedData::WatchStopReason::StoppedByUser));

        ASSERT_NE(model.entryOf(watchIdOf("watch")), nullptr);
        EXPECT_EQ(model.entryOf(watchIdOf("watch"))->name, "renamed.txt");
        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Stopped);
        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching), Names{});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Stopped), Names{"watch:watch"});
    }

    TEST(TrackingModelTests, EachGroupHoldsOnlyItsMembers)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        auto uploading = watchEntry("uploading", "uploading.txt");
        uploading.uploading = true;
        model.showEntry(uploading);
        model.showEntry(watchEntry("pending", "pending.txt"));
        model.showWatchProgress(progressOf("pending", 1u, 0u, 0u, 0u));
        model.showEntry(watchEntry("conflicting", "conflicting.txt"));
        model.showConflict(conflictOf("conflict", "conflicting", 100u));
        model.showEntry(watchEntry("failing", "failing.txt"));
        model.showFailure(failureOf("failure", "failing", 100u));
        model.showEntry(watchEntry("idle", "idle.txt"));
        model.showEntry(stoppedEntry("stopped", "stopped.txt", SharedData::WatchStopReason::CopyGone));
        model.showOrphanedCopies(orphansOf({orphanOf("orphan", "orphan.txt")}));

        EXPECT_EQ(membersOf(model, OutlinerGroup::NeedsYou), (Names{"watch:conflicting", "watch:failing"}));
        EXPECT_EQ(membersOf(model, OutlinerGroup::Syncing), (Names{"watch:pending", "watch:uploading"}));
        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching), Names{"watch:idle"});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Stopped), Names{"watch:stopped"});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned), Names{"instance:orphan"});
    }

    TEST(TrackingModelTests, MembersAreSortedByNameIgnoringCase)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("first", "gamma.txt"));
        model.showEntry(watchEntry("second", "Alpha.txt"));
        model.showEntry(watchEntry("third", "beta.txt"));
        model.showEntry(watchEntry("fourth", "alpha2.txt"));

        EXPECT_EQ(
            membersOf(model, OutlinerGroup::Watching),
            (Names{"watch:second", "watch:fourth", "watch:third", "watch:first"})
        );
    }

    TEST(TrackingModelTests, NamesThatDifferOnlyInCaseAreOrderedById)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("bravo", "Notes.txt"));
        model.showEntry(watchEntry("charlie", "NOTES.txt"));
        model.showEntry(watchEntry("alpha", "notes.txt"));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching), (Names{"watch:alpha", "watch:bravo", "watch:charlie"}));
    }

    TEST(TrackingModelTests, TheFilterKeepsNamesThatContainItIgnoringCase)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("report", "Report.pdf"));
        model.showEntry(watchEntry("notes", "notes.txt"));
        model.showEntry(watchEntry("old", "REPORT-old.pdf"));
        model.showEntry(watchEntry("summary", "summary.md"));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching, "report"), (Names{"watch:old", "watch:report"}));
        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching, "PORT"), (Names{"watch:old", "watch:report"}));
        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching, ".TXT"), Names{"watch:notes"});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching, "-old.p"), Names{"watch:old"});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching, "xyz"), Names{});
        EXPECT_EQ(
            membersOf(model, OutlinerGroup::Watching, ""),
            (Names{"watch:notes", "watch:old", "watch:report", "watch:summary"})
        );
    }

    TEST(TrackingModelTests, MatchesFilterComparesAsciiLettersWithoutCase)
    {
        EXPECT_TRUE(matchesFilter("Report.pdf", ""));
        EXPECT_TRUE(matchesFilter("", ""));
        EXPECT_TRUE(matchesFilter("Report.pdf", "report"));
        EXPECT_TRUE(matchesFilter("Report.pdf", "PDF"));
        EXPECT_TRUE(matchesFilter("Report.pdf", "Report.pdf"));
        EXPECT_TRUE(matchesFilter("\xc3\x84rger.txt", "\xc3\x84rger"));
        EXPECT_FALSE(matchesFilter("\xc3\x84rger.txt", "\xc3\xa4rger"));
        EXPECT_FALSE(matchesFilter("Report.pdf", "rep.pdf"));
        EXPECT_FALSE(matchesFilter("Report.pdf", "Report.pdf2"));
        EXPECT_FALSE(matchesFilter("", "a"));
    }

    TEST(TrackingModelTests, TheFilterAppliesToOrphansToo)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showOrphanedCopies(orphansOf({orphanOf("notes", "notes.txt"), orphanOf("report", "report.pdf")}));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned, "REP"), Names{"instance:report"});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned, "zzz"), Names{});
    }

    TEST(TrackingModelTests, OrphansAreSortedByNameIgnoringCaseTiesById)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showOrphanedCopies(
            orphansOf({orphanOf("charlie", "b.txt"), orphanOf("bravo", "a.txt"), orphanOf("alpha", "B.txt")})
        );

        EXPECT_EQ(
            membersOf(model, OutlinerGroup::Orphaned), (Names{"instance:bravo", "instance:alpha", "instance:charlie"})
        );
    }

    TEST(TrackingModelTests, OrphansAreOnlyMembersOfOrphaned)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showOrphanedCopies(orphansOf({orphanOf("orphan", "notes.txt")}));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Watching), Names{"watch:watch"});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned), Names{"instance:orphan"});
        for (auto const group : {OutlinerGroup::NeedsYou, OutlinerGroup::Syncing, OutlinerGroup::Stopped})
            EXPECT_EQ(membersOf(model, group), Names{});
    }

    TEST(TrackingModelTests, AStoppedCopyIsHiddenFromTheOrphansUntilItsEntryIsRemoved)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showOrphanedCopies(orphansOf({orphanOf("held", "notes.txt"), orphanOf("free", "other.txt")}));
        auto entry = copyEntry("watch", "notes.txt", "held");
        entry.stop = SharedData::WatchStop{.reason = SharedData::WatchStopReason::StoppedByUser, .stoppedAt = 100u};
        model.showEntry(entry);

        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned), Names{"instance:free"});
        EXPECT_EQ(membersOf(model, OutlinerGroup::Stopped), Names{"watch:watch"});
        EXPECT_EQ(model.orphanOf(Ids::makeInstanceId("held")), nullptr);
        EXPECT_NE(model.orphanOf(Ids::makeInstanceId("free")), nullptr);

        model.removeEntry(watchIdOf("watch"));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned), (Names{"instance:held", "instance:free"}));
        EXPECT_EQ(membersOf(model, OutlinerGroup::Stopped), Names{});
        ASSERT_NE(model.orphanOf(Ids::makeInstanceId("held")), nullptr);
        EXPECT_EQ(model.orphanOf(Ids::makeInstanceId("held"))->name, "notes.txt");
    }

    TEST(TrackingModelTests, ARunningCopyHoldsItsInstanceToo)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(copyEntry("watch", "notes.txt", "held"));
        model.showOrphanedCopies(orphansOf({orphanOf("held", "notes.txt")}));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned), Names{});
        EXPECT_EQ(model.orphanOf(Ids::makeInstanceId("held")), nullptr);
    }

    TEST(TrackingModelTests, OrphanedCopiesReplaceTheListShownBefore)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showOrphanedCopies(orphansOf({orphanOf("first", "first.txt"), orphanOf("second", "second.txt")}));
        model.showOrphanedCopies(orphansOf({orphanOf("third", "third.txt")}));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned), Names{"instance:third"});
        EXPECT_EQ(model.orphanOf(Ids::makeInstanceId("first")), nullptr);
        ASSERT_NE(model.orphanOf(Ids::makeInstanceId("third")), nullptr);
        EXPECT_EQ(model.orphanOf(Ids::makeInstanceId("third"))->name, "third.txt");
        EXPECT_EQ(model.orphanOf(Ids::makeInstanceId("unknown")), nullptr);

        model.showOrphanedCopies(orphansOf({}));

        EXPECT_EQ(membersOf(model, OutlinerGroup::Orphaned), Names{});
    }

    TEST(TrackingModelTests, ConflictsAndFailuresAreKeptBeforeTheirEntryArrives)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showConflict(conflictOf("conflict", "watch", 100u));
        model.showFailure(failureOf("failure", "watch", 100u));

        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("conflict")), watchIdOf("watch"));
        EXPECT_EQ(model.watchOfFailure(Ids::makeFailureId("failure")), watchIdOf("watch"));
        EXPECT_EQ(model.conflictCountOf(watchIdOf("watch")), 1u);
        EXPECT_EQ(model.failureCountOf(watchIdOf("watch")), 1u);
        EXPECT_EQ(model.conflictCount(), 0u);
        EXPECT_EQ(model.failureCount(), 0u);
        EXPECT_FALSE(model.newestConflict().has_value());
        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Watching);

        model.showEntry(watchEntry("watch", "notes.txt"));

        EXPECT_EQ(model.conflictCount(), 1u);
        EXPECT_EQ(model.failureCount(), 1u);
        EXPECT_EQ(model.statusOf(watchIdOf("watch")), WatchStatus::Conflicts);
        ASSERT_TRUE(model.newestConflict().has_value());
        EXPECT_EQ(model.newestConflict()->conflictId, Ids::makeConflictId("conflict"));
    }

    TEST(TrackingModelTests, RemovingAnEntryDropsItsConflictsFailuresAndProgress)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("removed", "removed.txt"));
        model.showEntry(watchEntry("kept", "kept.txt"));
        model.showConflict(conflictOf("removed-conflict", "removed", 100u));
        model.showConflict(conflictOf("kept-conflict", "kept", 100u));
        model.showFailure(failureOf("removed-failure", "removed", 100u));
        model.showFailure(failureOf("kept-failure", "kept", 100u));
        model.showWatchProgress(progressOf("removed", 1u, 0u, 0u, 0u));
        model.showWatchProgress(progressOf("kept", 1u, 0u, 0u, 0u));
        model.removeEntry(watchIdOf("removed"));

        EXPECT_EQ(model.entryOf(watchIdOf("removed")), nullptr);
        EXPECT_EQ(model.progressOf(watchIdOf("removed")), nullptr);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("removed-conflict")), std::nullopt);
        EXPECT_EQ(model.watchOfFailure(Ids::makeFailureId("removed-failure")), std::nullopt);
        EXPECT_TRUE(allConflictsOf(model, "removed").empty());
        EXPECT_TRUE(allFailuresOf(model, "removed").empty());
        EXPECT_EQ(model.conflictCount(), 1u);
        EXPECT_EQ(model.failureCount(), 1u);
        EXPECT_NE(model.progressOf(watchIdOf("kept")), nullptr);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("kept-conflict")), watchIdOf("kept"));
        EXPECT_EQ(membersOf(model, OutlinerGroup::NeedsYou), Names{"watch:kept"});

        model.showEntry(watchEntry("removed", "removed.txt"));

        EXPECT_EQ(model.conflictCountOf(watchIdOf("removed")), 0u);
        EXPECT_EQ(model.failureCountOf(watchIdOf("removed")), 0u);
        EXPECT_EQ(model.progressOf(watchIdOf("removed")), nullptr);
        EXPECT_EQ(model.groupOf(watchIdOf("removed")), OutlinerGroup::Watching);
    }

    TEST(TrackingModelTests, AConflictIsReplacedById)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("first", "first.txt"));
        model.showEntry(watchEntry("second", "second.txt"));
        model.showConflict(conflictOf("conflict", "first", 100u));
        auto replaced = conflictOf("conflict", "first", 200u);
        replaced.kind = SharedData::ConflictKind::DeletedOnServer;
        model.showConflict(replaced);

        const auto conflicts = allConflictsOf(model, "first");
        ASSERT_EQ(conflicts.size(), 1u);
        EXPECT_EQ(conflicts.front().raisedAt, 200u);
        EXPECT_EQ(conflicts.front().kind, SharedData::ConflictKind::DeletedOnServer);
        EXPECT_EQ(model.conflictCount(), 1u);

        model.showConflict(conflictOf("conflict", "second", 300u));

        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("conflict")), watchIdOf("second"));
        EXPECT_EQ(model.conflictCountOf(watchIdOf("first")), 0u);
        EXPECT_EQ(model.conflictCountOf(watchIdOf("second")), 1u);
    }

    TEST(TrackingModelTests, AFailureIsReplacedById)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showFailure(failureOf("failure", "watch", 100u));
        auto repeated = failureOf("failure", "watch", 300u);
        repeated.attempts = 3u;
        repeated.reason = SharedData::UploadFailureReason::NoSpaceOnServer;
        model.showFailure(repeated);

        const auto failures = allFailuresOf(model, "watch");
        ASSERT_EQ(failures.size(), 1u);
        EXPECT_EQ(failures.front().attempts, 3u);
        EXPECT_EQ(failures.front().failedAt, 300u);
        EXPECT_EQ(failures.front().reason, SharedData::UploadFailureReason::NoSpaceOnServer);
        EXPECT_EQ(model.failureCount(), 1u);
    }

    TEST(TrackingModelTests, RemovingUnknownItemsChangesNothing)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showConflict(conflictOf("conflict", "watch", 100u));
        model.showFailure(failureOf("failure", "watch", 100u));
        model.removeConflicts({Ids::makeConflictId("unknown")});
        model.removeFailures({Ids::makeFailureId("unknown")});
        model.removeEntry(watchIdOf("unknown"));

        EXPECT_EQ(model.conflictCount(), 1u);
        EXPECT_EQ(model.failureCount(), 1u);
        EXPECT_NE(model.entryOf(watchIdOf("watch")), nullptr);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("unknown")), std::nullopt);
        EXPECT_EQ(model.watchOfFailure(Ids::makeFailureId("unknown")), std::nullopt);
    }

    TEST(TrackingModelTests, TheConflictsOfAWatchStartWithTheLastShown)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showEntry(watchEntry("other", "other"));
        model.showConflict(conflictOf("first", "watch", 100u));
        model.showConflict(conflictOf("second", "watch", 300u));
        model.showConflict(conflictOf("third", "watch", 200u));
        model.showConflict(conflictOf("elsewhere", "other", 400u));

        EXPECT_EQ(conflictNamesOf(allConflictsOf(model, "watch")), (Names{"third", "second", "first"}));
        EXPECT_EQ(model.conflictCountOf(watchIdOf("watch")), 3u);
        EXPECT_EQ(model.conflictCountOf(watchIdOf("other")), 1u);
    }

    TEST(TrackingModelTests, AConflictShownAgainMovesToTheFront)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showConflict(conflictOf("alpha", "watch", 100u));
        model.showConflict(conflictOf("bravo", "watch", 100u));
        model.showConflict(conflictOf("charlie", "watch", 100u));
        model.showConflict(conflictOf("alpha", "watch", 200u));

        EXPECT_EQ(conflictNamesOf(allConflictsOf(model, "watch")), (Names{"alpha", "charlie", "bravo"}));
        EXPECT_EQ(model.conflictCountOf(watchIdOf("watch")), 3u);
        EXPECT_EQ(model.conflictCount(), 3u);
    }

    TEST(TrackingModelTests, TheFailuresOfAWatchStartWithTheLastShown)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showFailure(failureOf("alpha", "watch", 100u));
        model.showFailure(failureOf("bravo", "watch", 300u));
        model.showFailure(failureOf("charlie", "watch", 100u));
        model.showFailure(failureOf("delta", "other", 500u));

        EXPECT_EQ(failureNamesOf(allFailuresOf(model, "watch")), (Names{"charlie", "bravo", "alpha"}));
        EXPECT_EQ(model.failureCountOf(watchIdOf("watch")), 3u);

        auto repeated = failureOf("alpha", "watch", 600u);
        repeated.attempts = 2u;
        model.showFailure(repeated);

        EXPECT_EQ(failureNamesOf(allFailuresOf(model, "watch")), (Names{"alpha", "charlie", "bravo"}));
        EXPECT_EQ(model.failureCountOf(watchIdOf("watch")), 3u);
    }

    TEST(TrackingModelTests, PagesAreSlicesOfTheConflictsOfOneWatch)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showEntry(watchEntry("other", "other"));
        showConflicts(model, "conflict", "watch", 250);
        showConflicts(model, "other", "other", 3);

        const auto first = model.conflictsOf(watchIdOf("watch"), 0u, 100u);
        ASSERT_EQ(first.size(), 100u);
        EXPECT_EQ(first.front().conflictId, Ids::makeConflictId("conflict-249"));
        EXPECT_EQ(first.back().conflictId, Ids::makeConflictId("conflict-150"));

        const auto last = model.conflictsOf(watchIdOf("watch"), 200u, 100u);
        ASSERT_EQ(last.size(), 50u);
        EXPECT_EQ(last.front().conflictId, Ids::makeConflictId("conflict-49"));
        EXPECT_EQ(last.back().conflictId, Ids::makeConflictId("conflict-0"));

        EXPECT_TRUE(model.conflictsOf(watchIdOf("watch"), 250u, 100u).empty());
        EXPECT_TRUE(model.conflictsOf(watchIdOf("unknown"), 0u, 100u).empty());
        EXPECT_EQ(conflictNamesOf(model.conflictsOf(watchIdOf("other"), 1u, 100u)), (Names{"other-1", "other-0"}));
    }

    TEST(TrackingModelTests, PagesAreSlicesOfTheFailuresOfOneWatch)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        showFailures(model, "failure", "watch", 120);
        showFailures(model, "other", "other", 5);

        const auto second = model.failuresOf(watchIdOf("watch"), 100u, 100u);
        ASSERT_EQ(second.size(), 20u);
        EXPECT_EQ(second.front().failureId, Ids::makeFailureId("failure-19"));
        EXPECT_EQ(second.back().failureId, Ids::makeFailureId("failure-0"));
        EXPECT_EQ(model.failureCountOf(watchIdOf("watch")), 120u);
        EXPECT_EQ(model.failureCountOf(watchIdOf("other")), 5u);
        EXPECT_EQ(model.failureCount(), 120u);
    }

    TEST(TrackingModelTests, RemovingManyConflictsAcrossWatchesUpdatesEveryCount)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("first", "first"));
        model.showEntry(watchEntry("second", "second"));
        showConflicts(model, "first", "first", 4);
        showConflicts(model, "second", "second", 3);
        showConflicts(model, "unknown", "unknown", 2);

        model.removeConflicts(conflictIdsOf({"first-1", "second-0", "unknown-0", "first-3", "missing", "first-1"}));

        EXPECT_EQ(conflictNamesOf(allConflictsOf(model, "first")), (Names{"first-2", "first-0"}));
        EXPECT_EQ(conflictNamesOf(allConflictsOf(model, "second")), (Names{"second-2", "second-1"}));
        EXPECT_EQ(model.conflictCountOf(watchIdOf("unknown")), 1u);
        EXPECT_EQ(model.conflictCount(), 4u);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("first-1")), std::nullopt);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("unknown-0")), std::nullopt);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("first-2")), watchIdOf("first"));

        model.removeConflicts(conflictIdsOf({"second-2", "second-1"}));

        EXPECT_EQ(model.conflictCountOf(watchIdOf("second")), 0u);
        EXPECT_EQ(model.statusOf(watchIdOf("second")), WatchStatus::Watching);
        EXPECT_EQ(model.conflictCount(), 2u);
    }

    TEST(TrackingModelTests, RemovingManyFailuresAcrossWatchesUpdatesEveryCount)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("first", "first"));
        model.showEntry(watchEntry("second", "second"));
        showFailures(model, "first", "first", 3);
        showFailures(model, "second", "second", 2);

        model.removeFailures(failureIdsOf({"first-0", "second-1", "first-2", "missing"}));

        EXPECT_EQ(failureNamesOf(allFailuresOf(model, "first")), (Names{"first-1"}));
        EXPECT_EQ(failureNamesOf(allFailuresOf(model, "second")), (Names{"second-0"}));
        EXPECT_EQ(model.failureCount(), 2u);
        EXPECT_EQ(model.watchOfFailure(Ids::makeFailureId("first-0")), std::nullopt);

        model.removeFailures({});

        EXPECT_EQ(model.failureCount(), 2u);
    }

    TEST(TrackingModelTests, RemovingAWatchKeepsTheProblemsOfTheOthers)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("removed", "removed"));
        model.showEntry(watchEntry("kept", "kept"));
        showConflicts(model, "removed", "removed", 150);
        showFailures(model, "removed", "removed", 150);
        showConflicts(model, "kept", "kept", 2);
        showFailures(model, "kept", "kept", 3);
        model.removeEntry(watchIdOf("removed"));

        EXPECT_EQ(model.conflictCount(), 2u);
        EXPECT_EQ(model.failureCount(), 3u);
        EXPECT_EQ(model.conflictCountOf(watchIdOf("removed")), 0u);
        EXPECT_EQ(model.failureCountOf(watchIdOf("removed")), 0u);
        EXPECT_TRUE(model.conflictsOf(watchIdOf("removed"), 0u, 100u).empty());
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("removed-7")), std::nullopt);
        EXPECT_EQ(model.watchOfFailure(Ids::makeFailureId("removed-7")), std::nullopt);
        EXPECT_EQ(conflictNamesOf(allConflictsOf(model, "kept")), (Names{"kept-1", "kept-0"}));
        EXPECT_EQ(model.watchOfFailure(Ids::makeFailureId("kept-2")), watchIdOf("kept"));

        model.showConflict(conflictOf("removed-7", "removed", 100u));

        EXPECT_EQ(model.conflictCountOf(watchIdOf("removed")), 1u);
        EXPECT_EQ(model.conflictCount(), 2u);
    }

    TEST(TrackingModelTests, TheTotalsFollowWatchesThatArriveAfterTheirProblems)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        showConflicts(model, "late", "late", 3);
        showFailures(model, "late", "late", 2);

        EXPECT_EQ(model.conflictCount(), 0u);
        EXPECT_EQ(model.failureCount(), 0u);

        model.showEntry(watchEntry("late", "late"));
        model.showEntry(watchEntry("late", "late renamed"));

        EXPECT_EQ(model.conflictCount(), 3u);
        EXPECT_EQ(model.failureCount(), 2u);

        model.removeConflicts(conflictIdsOf({"late-0"}));
        model.removeEntry(watchIdOf("late"));

        EXPECT_EQ(model.conflictCount(), 0u);
        EXPECT_EQ(model.failureCount(), 0u);
    }

    TEST(TrackingModelTests, AConflictMovedToAnotherWatchCountsThere)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("known", "known"));
        model.showConflict(conflictOf("moving", "known", 100u));
        model.showConflict(conflictOf("staying", "known", 100u));
        model.showConflict(conflictOf("moving", "unknown", 200u));

        EXPECT_EQ(model.conflictCount(), 1u);
        EXPECT_EQ(model.conflictCountOf(watchIdOf("known")), 1u);
        EXPECT_EQ(model.conflictCountOf(watchIdOf("unknown")), 1u);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("moving")), watchIdOf("unknown"));

        model.showConflict(conflictOf("moving", "known", 300u));

        EXPECT_EQ(model.conflictCount(), 2u);
        EXPECT_EQ(model.conflictCountOf(watchIdOf("unknown")), 0u);
        EXPECT_EQ(conflictNamesOf(allConflictsOf(model, "known")), (Names{"moving", "staying"}));
    }

    TEST(TrackingModelTests, TheTotalsCountEveryKnownWatch)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("first", "first.txt"));
        model.showEntry(stoppedEntry("second", "second.txt", SharedData::WatchStopReason::StoppedByUser));
        model.showConflict(conflictOf("first-1", "first", 100u));
        model.showConflict(conflictOf("first-2", "first", 100u));
        model.showConflict(conflictOf("second-1", "second", 100u));
        model.showConflict(conflictOf("unknown-1", "unknown", 100u));
        model.showFailure(failureOf("second-1", "second", 100u));
        model.showFailure(failureOf("unknown-1", "unknown", 100u));

        EXPECT_EQ(model.conflictCount(), 3u);
        EXPECT_EQ(model.failureCount(), 1u);
    }

    TEST(TrackingModelTests, TheNewestConflictIsTheLastShownOfAKnownWatch)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("first", "first.txt"));
        model.showEntry(watchEntry("second", "second.txt"));
        model.showConflict(conflictOf("older", "first", 100u));
        model.showConflict(conflictOf("newer", "first", 50u));
        model.showConflict(conflictOf("newest", "second", 10u));
        model.showConflict(conflictOf("unknown", "unknown", 900u));

        ASSERT_TRUE(model.newestConflict().has_value());
        EXPECT_EQ(model.newestConflict()->conflictId, Ids::makeConflictId("newest"));
        EXPECT_EQ(model.newestConflict()->watchId, watchIdOf("second"));

        model.removeConflicts({Ids::makeConflictId("newest")});

        ASSERT_TRUE(model.newestConflict().has_value());
        EXPECT_EQ(model.newestConflict()->conflictId, Ids::makeConflictId("newer"));
        EXPECT_EQ(conflictNamesOf(allConflictsOf(model, "first")).front(), "newer");
    }

    TEST(TrackingModelTests, AConflictShownAgainBecomesTheNewest)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("first", "first.txt"));
        model.showEntry(watchEntry("second", "second.txt"));
        model.showConflict(conflictOf("bravo", "first", 100u));
        model.showConflict(conflictOf("alpha", "second", 100u));
        model.showConflict(conflictOf("bravo", "first", 100u));

        ASSERT_TRUE(model.newestConflict().has_value());
        EXPECT_EQ(model.newestConflict()->conflictId, Ids::makeConflictId("bravo"));
    }

    TEST(TrackingModelTests, NoConflictsMeanNoNewestConflict)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;

        EXPECT_FALSE(model.newestConflict().has_value());

        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showConflict(conflictOf("conflict", "watch", 100u));
        model.removeConflicts({Ids::makeConflictId("conflict")});

        EXPECT_FALSE(model.newestConflict().has_value());
    }

    TEST(TrackingModelTests, TheRingFollowsTheBytesWhenTheirTotalIsKnown)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showWatchProgress(progressOf("watch", 3u, 1u, 25u, 100u));

        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("watch")), 0.25);

        model.showWatchProgress(progressOf("watch", 0u, 4u, 100u, 100u));

        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("watch")), 1.0);
    }

    TEST(TrackingModelTests, TheRingFollowsTheChangesWithoutAByteTotal)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showWatchProgress(progressOf("watch", 1u, 3u, 0u, 0u));

        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("watch")), 0.75);

        model.showWatchProgress(progressOf("watch", 4u, 0u, 50u, 0u));

        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("watch")), 0.0);
    }

    TEST(TrackingModelTests, TheRingIsEmptyWithoutProgressOrChanges)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));

        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("watch")), 0.0);

        model.showWatchProgress(progressOf("watch", 0u, 0u, 0u, 0u));

        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("watch")), 0.0);
    }

    TEST(TrackingModelTests, TheRingNeverPassesFull)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showWatchProgress(progressOf("watch", 1u, 0u, 150u, 100u));

        EXPECT_DOUBLE_EQ(model.ringFractionOf(watchIdOf("watch")), 1.0);
    }

    TEST(TrackingModelTests, NewProgressReplacesTheOld)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "project"));
        model.showWatchProgress(progressOf("watch", 5u, 0u, 10u, 100u));
        model.showWatchProgress(progressOf("watch", 2u, 3u, 60u, 100u));

        ASSERT_NE(model.progressOf(watchIdOf("watch")), nullptr);
        EXPECT_EQ(model.progressOf(watchIdOf("watch"))->changesPending, 2u);
        EXPECT_EQ(model.progressOf(watchIdOf("watch"))->changesDone, 3u);
        EXPECT_EQ(model.progressOf(watchIdOf("watch"))->bytesDone, 60u);
    }

    TEST(TrackingModelTests, ClearForgetsEverything)
    {
        using namespace TrackingModelTestDetail;
        TrackingModel model;
        model.showEntry(watchEntry("watch", "notes.txt"));
        model.showEntry(stoppedEntry("stopped", "stopped.txt", SharedData::WatchStopReason::ConnectionLost));
        model.showConflict(conflictOf("conflict", "watch", 100u));
        model.showFailure(failureOf("failure", "watch", 100u));
        model.showWatchProgress(progressOf("watch", 1u, 0u, 0u, 0u));
        model.showOrphanedCopies(orphansOf({orphanOf("orphan", "orphan.txt")}));
        model.clear();

        EXPECT_EQ(model.entryOf(watchIdOf("watch")), nullptr);
        EXPECT_EQ(model.progressOf(watchIdOf("watch")), nullptr);
        EXPECT_EQ(model.orphanOf(Ids::makeInstanceId("orphan")), nullptr);
        EXPECT_EQ(model.watchOfConflict(Ids::makeConflictId("conflict")), std::nullopt);
        EXPECT_EQ(model.watchOfFailure(Ids::makeFailureId("failure")), std::nullopt);
        EXPECT_EQ(model.conflictCount(), 0u);
        EXPECT_EQ(model.failureCount(), 0u);
        EXPECT_FALSE(model.newestConflict().has_value());
        for (auto const group : outlinerGroupOrder)
            EXPECT_EQ(membersOf(model, group), Names{});
    }
}
