#pragma once

#include <backend/sftp/bulk_upload_operation.hpp>
#include <backend/sftp/compared_state_check.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <constants/sftp.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/fake_remote_file_system.hpp>
#include <utility/temporary_directory.hpp>
#include "operation_runs.hpp"
#include "real_server_tests.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    /**
     * @brief The checks of an upload: ProbeEntry where the file system answers it, and nothing but the unsafe fast mode
     *        where it cannot, as on an FTP server without MLST and SIZE.
     */
    class UploadCheckTests : public ::testing::Test
    {
      protected:
        UploadCheckTests()
        {
            processingThread_.start(std::chrono::milliseconds{1});
        }
        ~UploadCheckTests() override
        {
            processingThread_.stop();
        }
        UploadCheckTests(UploadCheckTests const&) = delete;
        UploadCheckTests& operator=(UploadCheckTests const&) = delete;
        UploadCheckTests(UploadCheckTests&&) = delete;
        UploadCheckTests& operator=(UploadCheckTests&&) = delete;

        std::shared_ptr<RemoteIo::Test::FakeRemoteFileSystem> makeFake(bool probesEntries)
        {
            auto fake = std::make_shared<RemoteIo::Test::FakeRemoteFileSystem>(
                strand_.get(),
                RemoteIo::Test::FakeRemoteFileSystem::Options{.capabilities = {.probeEntries = probesEntries}}
            );
            fake->addDirectory(remote("/home/test"));
            return fake;
        }

        Paths::LocalPath writeLocalFile(std::string const& name, std::string const& content) const
        {
            const auto local = localDirectory_.path() / name;
            std::filesystem::create_directories(local.parent_path());
            std::ofstream{local, std::ios::binary} << content;
            return toLocal(local);
        }

        UploadOperation::UploadOperationOptions
        uploadOf(std::string const& name, std::string const& content, bool unsafeFast) const
        {
            return {
                .remotePath = remote("/home/test/" + name),
                .localPath = writeLocalFile(name, content),
                .unsafeFastMode = [unsafeFast]()
                {
                    return unsafeFast;
                },
            };
        }

        /**
         * @brief ComparedStateCheck::requireUnchangedRemoteInStrand of @p path on the strand of @p fake.
         */
        static std::expected<void, Operation::Error> checkUnchanged(
            RemoteIo::Test::FakeRemoteFileSystem& fake,
            std::string const& path,
            SharedData::ComparedState const& compared
        )
        {
            return fake
                .performPromise(
                    [&fake, &path, &compared]()
                    {
                        return ComparedStateCheck::requireUnchangedRemoteInStrand(fake, remote(path), compared);
                    }
                )
                .get();
        }

        /**
         * @brief What a sync compare saw of a 3 byte file whose time a listing showed to the minute.
         */
        static SharedData::ComparedState listedByMinute(std::uint64_t mtime)
        {
            return SharedData::ComparedState{
                .exists = true,
                .type = SharedData::FileType::Regular,
                .size = 3u,
                .mtime = mtime - mtime % 60u,
                .mtimePrecision = SharedData::TimePrecision::Minute,
            };
        }

        /**
         * @brief Records the ProbeEntry operations @p fake starts from now on.
         */
        static std::shared_ptr<std::vector<RemoteIo::ProbeEntry>>
        recordProbes(RemoteIo::Test::FakeRemoteFileSystem& fake)
        {
            auto probes = std::make_shared<std::vector<RemoteIo::ProbeEntry>>();
            fake.failWhen<RemoteIo::ProbeEntry>(
                [probes](RemoteIo::ProbeEntry const& probe)
                {
                    probes->push_back(probe);
                    return false;
                },
                {}
            );
            return probes;
        }

        static bool probedAnyEntryAt(std::vector<RemoteIo::ProbeEntry> const& probes, std::string const& path)
        {
            return std::ranges::any_of(
                probes,
                [&path](RemoteIo::ProbeEntry const& probe)
                {
                    return probe.path == remote(path) && probe.lookingFor == RemoteIo::ProbeFor::AnyEntry;
                }
            );
        }

        /** @brief 2023-11-14 22:13:57 UTC. */
        static constexpr std::uint64_t fileTime{1700000037u};

        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};
        RemoteIo::ProcessingThread processingThread_{};
        std::unique_ptr<RemoteIo::ProcessingStrand> strand_{processingThread_.createStrand()};
    };

    TEST_F(UploadCheckTests, ASafeUploadChecksWithProbesAndListsNothing)
    {
        const auto fake = makeFake(true);
        UploadOperation operation{fake, uploadOf("safe.txt", "safe content", false)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake->contentOf(remote("/home/test/safe.txt")), "safe content");
        EXPECT_GT(fake->started<RemoteIo::ProbeEntry>(), 0u);
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
    }

    TEST_F(UploadCheckTests, WithoutProbesASafeUploadIsRefusedBeforeItTouchesTheServer)
    {
        const auto fake = makeFake(false);
        UploadOperation operation{fake, uploadOf("safe.txt", "safe content", false)};

        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::CannotCheckDestination);
        EXPECT_TRUE(result.error().extraInfo.has_value());
        EXPECT_EQ(fake->started<RemoteIo::OpenFile>(), 0u);
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
        EXPECT_FALSE(fake->exists(remote("/home/test/safe.txt")));
    }

    TEST_F(UploadCheckTests, WithoutProbesAnUnsafeFastUploadReplacesAnExistingFile)
    {
        const auto fake = makeFake(false);
        fake->addFile(remote("/home/test/kept.txt"), "old content");
        UploadOperation operation{fake, uploadOf("kept.txt", "new content", true)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake->contentOf(remote("/home/test/kept.txt")), "new content");
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
        EXPECT_EQ(fake->started<RemoteIo::ProbeEntry>(), 0u);
    }

    TEST_F(UploadCheckTests, WithoutProbesAnUnsafeFastUploadOfAnySizeGoesStraightToItsDestination)
    {
        const auto fake = makeFake(false);
        const std::string content(Constants::unsafeFastSizeLimit, 'x');
        UploadOperation operation{fake, uploadOf("big.bin", content, true)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake->contentOf(remote("/home/test/big.bin")), content);
        EXPECT_FALSE(fake->exists(remote("/home/test/big.bin.filepart")));
        EXPECT_EQ(fake->started<RemoteIo::Rename>(), 0u);
    }

    TEST_F(UploadCheckTests, WithoutProbesAFolderUploadIsRefusedOnceAndCreatesNothing)
    {
        const auto fake = makeFake(false);
        for (auto const* name : {"tree/a.txt", "tree/b.txt", "tree/sub/c.txt"})
            writeLocalFile(name, name);
        const auto local = toLocal(localDirectory_.path() / "tree");
        BulkUploadOperation operation{
            fake,
            {
                .remotePath = remote("/home/test/tree"),
                .localPath = local,
                .individualOptions = {
                    .unsafeFastMode = []()
                    {
                        return false;
                    }
                },
            }
        };
        operation.scanFirst(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{.localPath = local})
        );

        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::CannotCheckDestination);
        EXPECT_FALSE(fake->exists(remote("/home/test/tree")));
        EXPECT_EQ(fake->started<RemoteIo::OpenFile>(), 0u);
    }

    TEST_F(UploadCheckTests, ASyncedFileIsCheckedWithTheProbeAtThePrecisionOfItsListing)
    {
        const auto fake = makeFake(true);
        fake->addFile(remote("/home/test/synced.txt"), "old");
        fake->setModificationTime(remote("/home/test/synced.txt"), fileTime);
        auto options = uploadOf("synced.txt", "new content", false);
        options.mayOverwrite = true;
        options.comparedDestination = listedByMinute(fileTime);
        UploadOperation operation{fake, std::move(options)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake->contentOf(remote("/home/test/synced.txt")), "new content");
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
    }

    TEST_F(UploadCheckTests, AProbedTimeOnAnotherClockIsCheckedAgainstTheListing)
    {
        RemoteIo::Test::FakeRemoteFileSystem::Options options{.capabilities = {.probeEntries = true}};
        options.probeTimeShift = 2u * 60u * 60u;
        const auto fake = std::make_shared<RemoteIo::Test::FakeRemoteFileSystem>(strand_.get(), options);
        fake->addFile(remote("/home/test/synced.txt"), "old");
        fake->setModificationTime(remote("/home/test/synced.txt"), fileTime);

        const auto checked = checkUnchanged(*fake, "/home/test/synced.txt", listedByMinute(fileTime));

        EXPECT_TRUE(checked.has_value()) << checked.error().toString();
        EXPECT_EQ(fake->started<RemoteIo::ProbeEntry>(), 1u);
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 1u);
    }

    TEST_F(UploadCheckTests, AChangeTheProbeCannotHaveMadeIsChangedWithoutTheListing)
    {
        const auto fake = makeFake(true);
        fake->addFile(remote("/home/test/synced.txt"), "four");
        fake->setModificationTime(remote("/home/test/synced.txt"), fileTime);
        auto exact = listedByMinute(fileTime);
        exact.size = 4u;
        exact.mtimePrecision = SharedData::TimePrecision::Second;

        const auto otherSize = checkUnchanged(*fake, "/home/test/synced.txt", listedByMinute(fileTime));
        const auto otherTime = checkUnchanged(*fake, "/home/test/synced.txt", exact);

        ASSERT_FALSE(otherSize.has_value());
        EXPECT_EQ(otherSize.error().type, Operation::ErrorType::ChangedSinceCompare);
        ASSERT_FALSE(otherTime.has_value());
        EXPECT_EQ(otherTime.error().type, Operation::ErrorType::ChangedSinceCompare);
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
    }

    TEST_F(UploadCheckTests, OnlyADifferenceTheProbeMayHaveMadeAsksTheListing)
    {
        using SharedData::ComparedState;
        using SharedData::FileType;
        using SharedData::TimePrecision;
        const ComparedState exactFile{.exists = true, .type = FileType::Regular, .size = 3u, .mtime = fileTime};
        auto laterFile = exactFile;
        laterFile.mtime += 60u;
        auto listedFile = exactFile;
        listedFile.mtimePrecision = TimePrecision::Minute;
        auto untimedFile = exactFile;
        untimedFile.mtime = 0u;
        const ComparedState link{.exists = true, .type = FileType::Symlink, .linkTarget = "a"};

        EXPECT_FALSE(ComparedStateCheck::probeMayMislead(exactFile, laterFile)) << "both times are exact";
        EXPECT_TRUE(ComparedStateCheck::probeMayMislead(listedFile, laterFile)) << "a listing may read another clock";
        EXPECT_TRUE(ComparedStateCheck::probeMayMislead(exactFile, untimedFile)) << "the probe told no time";
        EXPECT_TRUE(ComparedStateCheck::probeMayMislead(link, exactFile)) << "the probe may follow the link";
        EXPECT_FALSE(ComparedStateCheck::probeMayMislead(ComparedState{}, exactFile)) << "something appeared";
        auto biggerFile = laterFile;
        biggerFile.size = 4u;
        EXPECT_FALSE(ComparedStateCheck::probeMayMislead(listedFile, biggerFile)) << "the size is exact";
    }

    TEST_F(UploadCheckTests, ASmallFileIsCheckedForFilesOnlyAndOnlyBeforeTheRename)
    {
        const auto fake = makeFake(true);
        const auto probes = recordProbes(*fake);
        UploadOperation operation{fake, uploadOf("small.txt", std::string(4u * 1024u, 's'), false)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        ASSERT_EQ(probes->size(), 2u);
        EXPECT_EQ(probes->at(0).path, remote("/home/test/small.txt.filepart"));
        EXPECT_EQ(probes->at(0).lookingFor, RemoteIo::ProbeFor::File);
        EXPECT_EQ(probes->at(1).path, remote("/home/test/small.txt"));
        EXPECT_EQ(probes->at(1).lookingFor, RemoteIo::ProbeFor::File);
    }

    TEST_F(UploadCheckTests, ALargerFileOrTheOptionOffChecksTheDestinationFirst)
    {
        const auto fake = makeFake(true);
        const auto probes = recordProbes(*fake);
        UploadOperation larger{fake, uploadOf("larger.txt", std::string(4u * 1024u + 1u, 'l'), false)};
        auto options = uploadOf("checked.txt", "small", false);
        options.uncheckedUpToBytes = std::nullopt;
        UploadOperation checked{fake, std::move(options)};

        ASSERT_TRUE(runToEnd(larger).has_value());
        ASSERT_TRUE(runToEnd(checked).has_value());

        EXPECT_TRUE(probedAnyEntryAt(*probes, "/home/test/larger.txt"));
        EXPECT_TRUE(probedAnyEntryAt(*probes, "/home/test/checked.txt"));
    }

    TEST_F(UploadCheckTests, AnUploadThatMayOverwriteChecksNothingBeforeItSends)
    {
        const auto fake = makeFake(true);
        fake->addFile(remote("/home/test/replaced.txt"), "old content");
        auto options = uploadOf("replaced.txt", std::string(64u * 1024u, 'r'), false);
        options.mayOverwrite = true;
        UploadOperation operation{fake, std::move(options)};
        const auto probes = recordProbes(*fake);
        ASSERT_TRUE(operation.prepare().has_value());

        EXPECT_FALSE(probedAnyEntryAt(*probes, "/home/test/replaced.txt"));
        EXPECT_EQ(fake->contentOf(remote("/home/test/replaced.txt")), "old content");
    }

    TEST_F(UploadCheckTests, ASmallFileOntoAnExistingOneIsRefusedAtTheRenameAndLeavesNoPart)
    {
        const auto fake = makeFake(true);
        fake->addFile(remote("/home/test/kept.txt"), "kept content");
        UploadOperation operation{fake, uploadOf("kept.txt", "new content", false)};

        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::FileExists);
        EXPECT_EQ(fake->contentOf(remote("/home/test/kept.txt")), "kept content");
        EXPECT_FALSE(fake->exists(remote("/home/test/kept.txt.filepart")));
    }
}
