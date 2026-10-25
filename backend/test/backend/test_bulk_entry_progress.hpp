#pragma once

#include <backend/file_tracking/bulk_entry_progress.hpp>
#include <backend/sftp/bulk_upload_operation.hpp>
#include "real_server_tests.hpp"

#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test::BulkEntryProgressTest
{
    using FileTracking::BulkEntryProgress;
    using FileTracking::UploadProgress;

    namespace BulkEntryProgressTestDetail
    {
        inline Paths::LocalPath local(std::string_view path)
        {
            return Paths::LocalPath::fromUtf8(path, Paths::LocalPlatform::Posix).value();
        }

        inline SharedData::BulkFileProgress
        travelling(std::uint64_t entryIndex, std::uint64_t bytes, std::uint64_t totalBytes, std::string file = "/srv/f")
        {
            return {.file = std::move(file), .entryIndex = entryIndex, .bytes = bytes, .totalBytes = totalBytes};
        }

        inline SharedData::BulkProgress snapshot(
            std::uint64_t finished,
            std::uint64_t count,
            std::vector<SharedData::BulkFileProgress> inFlight,
            std::vector<std::uint32_t> leftOut = {}
        )
        {
            return {
                .operationId = Ids::generateOperationId(),
                .fileCurrentIndex = finished,
                .fileCount = count,
                .bytesCurrent = 0u,
                .bytesTotal = 0u,
                .bytesPerSecond = 0,
                .inFlight = std::move(inFlight),
                .failedEntryIndices = std::move(leftOut),
            };
        }

        inline BulkEntryProgress twoFiles()
        {
            return BulkEntryProgress::ofFiles({
                {.source = local("/home/user/a.txt"), .sizeBytes = 10u},
                {.source = local("/home/user/b.txt"), .sizeBytes = 20u},
            });
        }

        /**
         * @brief What was told, the last progress of each index.
         */
        inline std::map<std::size_t, UploadProgress> byIndex(std::vector<BulkEntryProgress::EntryProgress> const& told)
        {
            std::map<std::size_t, UploadProgress> last;
            for (auto const& entry : told)
                last.insert_or_assign(entry.index, entry.progress);
            return last;
        }
    }

    using namespace BulkEntryProgressTestDetail;

    TEST(BulkEntryProgressTests, AFileOnItsWayTellsItsBytesUnderItsLocalPathAndTheOthersWait)
    {
        auto progress = twoFiles();

        const auto told = progress.update(snapshot(0u, 2u, {travelling(0u, 4u, 10u)}));

        ASSERT_EQ(told.size(), 1u);
        EXPECT_EQ(told[0].index, 0u);
        EXPECT_EQ(told[0].progress.bytesSent, 4u);
        EXPECT_EQ(told[0].progress.bytesTotal, 10u);
        EXPECT_EQ(told[0].progress.filesDone, 0u);
        EXPECT_EQ(told[0].progress.fileCount, 1u);
        EXPECT_FALSE(told[0].progress.transferred);
        ASSERT_EQ(told[0].progress.inFlight.size(), 1u);
        EXPECT_EQ(told[0].progress.inFlight[0].path, local("/home/user/a.txt"));
        EXPECT_EQ(told[0].progress.inFlight[0].bytes, 4u);
        EXPECT_EQ(told[0].progress.inFlight[0].totalBytes, 10u);
    }

    TEST(BulkEntryProgressTests, AFileNoLongerOnItsWayIsDoneAndNothingIsToldTwice)
    {
        auto progress = twoFiles();
        std::ignore = progress.update(snapshot(0u, 2u, {travelling(0u, 4u, 10u)}));

        const auto told = byIndex(progress.update(snapshot(1u, 2u, {travelling(1u, 5u, 20u)})));

        ASSERT_EQ(told.size(), 2u);
        EXPECT_TRUE(told.at(0).transferred);
        EXPECT_EQ(told.at(0).bytesSent, 10u);
        EXPECT_EQ(told.at(0).filesDone, 1u);
        EXPECT_TRUE(told.at(0).inFlight.empty());
        EXPECT_EQ(told.at(1).bytesSent, 5u);
        EXPECT_FALSE(told.at(1).transferred);

        EXPECT_TRUE(progress.update(snapshot(1u, 2u, {travelling(1u, 5u, 20u)})).empty());
    }

    TEST(BulkEntryProgressTests, AFileLeftOutKeepsWhatWentAndIsNotDone)
    {
        auto progress = twoFiles();
        std::ignore = progress.update(snapshot(0u, 2u, {travelling(0u, 3u, 10u)}));

        const auto told = progress.update(snapshot(1u, 2u, {}, {0u}));

        ASSERT_EQ(told.size(), 1u);
        EXPECT_EQ(told[0].index, 0u);
        EXPECT_FALSE(told[0].progress.transferred);
        EXPECT_EQ(told[0].progress.bytesSent, 3u);
        EXPECT_EQ(told[0].progress.filesDone, 0u);
        EXPECT_TRUE(told[0].progress.inFlight.empty());
    }

    TEST(BulkEntryProgressTests, FilesNeverSeenOnTheirWayAreDoneOnceTheBulkIsThroughExceptThoseLeftOut)
    {
        auto progress = twoFiles();
        EXPECT_TRUE(progress.update(snapshot(0u, 2u, {})).empty());

        const auto told = byIndex(progress.update(snapshot(2u, 2u, {}, {1u})));

        ASSERT_EQ(told.size(), 2u);
        EXPECT_TRUE(told.at(0).transferred);
        EXPECT_EQ(told.at(0).bytesSent, 10u);
        EXPECT_FALSE(told.at(1).transferred);
        EXPECT_EQ(told.at(1).bytesSent, 0u);
        EXPECT_EQ(told.at(1).bytesTotal, 20u);
    }

    TEST(BulkEntryProgressTests, TheSizeTheTransferTellsReplacesTheQueuedOne)
    {
        auto progress = twoFiles();
        std::ignore = progress.update(snapshot(0u, 2u, {travelling(0u, 12u, 12u)}));

        const auto told = byIndex(progress.update(snapshot(1u, 2u, {})));

        ASSERT_TRUE(told.contains(0));
        EXPECT_EQ(told.at(0).bytesSent, 12u);
        EXPECT_EQ(told.at(0).bytesTotal, 12u);
    }

    TEST(BulkEntryProgressTests, EntriesBeyondTheRequestsAreIgnored)
    {
        auto progress = twoFiles();

        EXPECT_TRUE(progress.update(snapshot(0u, 2u, {travelling(5u, 1u, 2u)}, {7u})).empty());
    }

    TEST(BulkEntryProgressTests, ADirectoryTellsItsEntriesAndItsFilesOnTheirWayByLocalPath)
    {
        auto progress = BulkEntryProgress::ofDirectory(local("/home/user/project"), remote("/srv/project"));
        auto running = snapshot(
            1u,
            3u,
            {travelling(2u, 3u, 10u, "/srv/project/src/main.cpp"), travelling(3u, 1u, 4u, "/elsewhere/other.txt")}
        );
        running.bytesCurrent = 5u;
        running.bytesTotal = 30u;

        const auto told = progress.update(running);

        ASSERT_EQ(told.size(), 1u);
        EXPECT_EQ(told[0].index, 0u);
        EXPECT_EQ(told[0].progress.bytesSent, 5u);
        EXPECT_EQ(told[0].progress.bytesTotal, 30u);
        EXPECT_EQ(told[0].progress.filesDone, 1u);
        EXPECT_EQ(told[0].progress.fileCount, 3u);
        EXPECT_FALSE(told[0].progress.transferred);
        ASSERT_EQ(told[0].progress.inFlight.size(), 1u);
        EXPECT_EQ(told[0].progress.inFlight[0].path, local("/home/user/project/src/main.cpp"));
        EXPECT_EQ(told[0].progress.inFlight[0].bytes, 3u);
        EXPECT_EQ(told[0].progress.inFlight[0].totalBytes, 10u);

        EXPECT_TRUE(progress.update(running).empty());

        auto through = snapshot(3u, 3u, {});
        through.bytesCurrent = 30u;
        through.bytesTotal = 30u;
        const auto ended = progress.update(through);
        ASSERT_EQ(ended.size(), 1u);
        EXPECT_TRUE(ended[0].progress.transferred);
        EXPECT_TRUE(ended[0].progress.inFlight.empty());
        EXPECT_EQ(ended[0].progress.bytesSent, 30u);
    }

    TEST(BulkEntryProgressTests, ADirectoryNotScannedYetIsNotTransferred)
    {
        auto progress = BulkEntryProgress::ofDirectory(local("/home/user/project"), remote("/srv/project"));

        const auto told = progress.update(snapshot(0u, 0u, {}));

        ASSERT_EQ(told.size(), 1u);
        EXPECT_FALSE(told[0].progress.transferred);
    }

    TEST(BulkEntryProgressTests, ASingleTransferTellsItsPositionBetweenItsBounds)
    {
        const auto source = local("/home/user/a.txt");
        const auto running = FileTracking::uploadProgressOf(
            {.operationId = {}, .min = 0u, .max = 100u, .current = 40u, .bytesPerSecond = 0}, source
        );
        EXPECT_EQ(running.bytesSent, 40u);
        EXPECT_EQ(running.bytesTotal, 100u);
        EXPECT_EQ(running.filesDone, 0u);
        EXPECT_EQ(running.fileCount, 1u);
        EXPECT_FALSE(running.transferred);
        ASSERT_EQ(running.inFlight.size(), 1u);
        EXPECT_EQ(running.inFlight[0].path, source);
        EXPECT_EQ(running.inFlight[0].bytes, 40u);
        EXPECT_EQ(running.inFlight[0].totalBytes, 100u);

        const auto sent = FileTracking::uploadProgressOf(
            {.operationId = {}, .min = 0u, .max = 100u, .current = 100u, .bytesPerSecond = 0}, source
        );
        EXPECT_TRUE(sent.transferred);
        EXPECT_EQ(sent.bytesSent, 100u);
        EXPECT_EQ(sent.filesDone, 1u);
        EXPECT_TRUE(sent.inFlight.empty());

        // A link or a vanished source shows as done without bytes.
        const auto link = FileTracking::uploadProgressOf(
            {.operationId = {}, .min = 1u, .max = 1u, .current = 1u, .bytesPerSecond = 0}, source
        );
        EXPECT_TRUE(link.transferred);
        EXPECT_EQ(link.bytesSent, 0u);
        EXPECT_EQ(link.bytesTotal, 0u);
    }

    class BulkEntryProgressServerTests : public RealServerTests
    {
      protected:
        static void writeFile(std::filesystem::path const& path, std::string_view contents)
        {
            std::ofstream stream{path, std::ios::binary};
            stream << contents;
        }

        Utility::TemporaryDirectory sources_{temporaryRoot, true};
    };

    TEST_F(BulkEntryProgressServerTests, TheEntryIndexOfABulkOfFilesIsTheIndexOfItsRequest)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // Not in name order, the third source is gone by the time its turn comes.
        const std::vector<std::pair<std::string, std::string>> requests{
            {"zeta.txt", "zeta content"},
            {"alpha.txt", "alpha"},
            {"gone.txt", ""},
            {"mid.txt", "the middle one"},
        };
        std::vector<BulkUploadOperation::PrescannedFile> files;
        std::vector<BulkEntryProgress::File> uploads;
        std::vector<std::string> destinations;
        for (auto const& [name, content] : requests)
        {
            if (name != "gone.txt")
                writeFile(sources_.path() / name, content);
            const auto destination = "/home/test/entry_index_" + name;
            files.push_back({
                .source = toLocal(sources_.path() / name),
                .destination = remote(destination),
                .sizeBytes = content.size(),
                .mayOverwrite = true,
            });
            uploads.push_back({.source = toLocal(sources_.path() / name), .sizeBytes = content.size()});
            destinations.push_back(destination);
        }

        auto entryProgress = BulkEntryProgress::ofFiles(uploads);
        std::vector<SharedData::BulkProgress> snapshots;
        std::map<std::size_t, UploadProgress> told;
        UploadOperation::UploadOperationOptions individualOptions{};
        individualOptions.sourceMayVanish = true;
        individualOptions.createMissingDirectories = true;
        BulkUploadOperation operation{
            sftp,
            {
                .overallProgressCallback =
                    [&](SharedData::BulkProgress const& update)
                {
                    snapshots.push_back(update);
                    for (auto const& entry : entryProgress.update(update))
                        told.insert_or_assign(entry.index, entry.progress);
                },
                .remotePath = remote("/home/test"),
                .localPath = toLocal(sources_.path()),
                .individualOptions = std::move(individualOptions),
                .concurrency = 2,
                .progressEmitInterval = std::chrono::milliseconds{0},
            }
        };
        operation.setPrescannedFileList(std::move(files));

        for (int step = 0; step < 2000; ++step)
        {
            const auto worked = operation.work();
            ASSERT_TRUE(worked.has_value()) << worked.error().toString();
            if (*worked == BulkUploadOperation::WorkStatus::Complete)
                break;
        }

        std::set<std::uint64_t> seen;
        for (auto const& update : snapshots)
            for (auto const& file : update.inFlight)
            {
                ASSERT_LT(file.entryIndex, destinations.size());
                EXPECT_EQ(file.file, destinations[file.entryIndex]);
                seen.insert(file.entryIndex);
            }
        EXPECT_EQ(seen, (std::set<std::uint64_t>{0u, 1u, 2u, 3u}));
        ASSERT_FALSE(snapshots.empty());
        EXPECT_EQ(snapshots.back().failedEntryIndices, std::vector<std::uint32_t>{2u});
        ASSERT_EQ(operation.indexedFailures().size(), 1u);
        EXPECT_EQ(operation.indexedFailures().front().index, 2u);

        ASSERT_EQ(told.size(), 4u);
        for (std::size_t index : {0u, 1u, 3u})
        {
            EXPECT_TRUE(told.at(index).transferred) << requests[index].first;
            EXPECT_EQ(told.at(index).bytesSent, requests[index].second.size()) << requests[index].first;
        }
        EXPECT_FALSE(told.at(2u).transferred);
    }
}
