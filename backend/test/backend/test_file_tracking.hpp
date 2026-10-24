#pragma once

#include <backend/file_tracking/instance_bookkeeping.hpp>
#include <backend/file_tracking/instance_lock.hpp>
#include <backend/file_tracking/instance_watch.hpp>
#include <backend/file_tracking/temp_dir_instancing.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <paths/native.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>

#include <nlohmann/json.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace FileTracking;
using namespace std::chrono_literals;

extern std::filesystem::path programDirectory;
extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace fs = std::filesystem;

    // -------------------------------------------------------------------------
    // Shared helpers
    // -------------------------------------------------------------------------

    /**
     * @brief Create a uniquely-named temp directory and return its path.
     *
     * @param tag  Short tag appended to the directory name for debugging.
     * @return     The created directory path.
     */
    inline fs::path makeTestTempRoot(std::string const& tag = "base")
    {
        auto suffix = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        auto root = temporaryRoot / ("nui_sftp_ft_" + tag + "_" + suffix);
        fs::create_directories(root);
        return root;
    }

    /**
     * @brief Reads a JSON file, an empty json when it cannot be read.
     */
    inline nlohmann::json readJson(fs::path const& path)
    {
        std::ifstream input{path};
        return nlohmann::json::parse(input, nullptr, false);
    }

    /**
     * @brief The content of a text file.
     */
    inline std::string readText(fs::path const& path)
    {
        std::ifstream input{path};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    /**
     * @brief @p path as a remote path, it must be a valid one.
     */
    inline Paths::RemotePath remotePathOf(std::string_view path)
    {
        return Paths::RemotePath::make(path).value();
    }

    /**
     * @brief Writes @p size bytes into @p path.
     */
    inline void writeFileOfSize(fs::path const& path, std::size_t size)
    {
        std::ofstream{path, std::ios::binary} << std::string(size, 'x');
    }

    /**
     * @brief The copy of the instance @p instanceId in @p copies, nothing when it is not listed.
     */
    inline std::optional<SharedData::OrphanedCopy>
    listedCopyOf(std::vector<SharedData::OrphanedCopy> const& copies, std::string const& instanceId)
    {
        const auto found = std::ranges::find_if(
            copies,
            [&instanceId](SharedData::OrphanedCopy const& copy)
            {
                return copy.instanceId.value() == instanceId;
            }
        );
        if (found == copies.end())
            return std::nullopt;
        return *found;
    }

    /**
     * @brief Writes a metadata file with the given @p deadAt (or null if absent).
     */
    inline void writeTestMetadata(
        fs::path const& path,
        std::string const& instanceId,
        std::optional<std::chrono::system_clock::time_point> deadAt
    )
    {
        nlohmann::json meta = {
            {"instanceId", instanceId},
            {"createdAt", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"deadAt", nullptr},
        };
        if (deadAt)
            meta["deadAt"] = std::chrono::system_clock::to_time_t(*deadAt);
        std::ofstream{path} << meta.dump(4);
    }

    /**
     * @brief Simulate the on-disk remnants of a dead TemporaryDirectoryInstance of this build.
     *
     * Creates @p root / @p instanceId / with an unowned lock file and a metadata file in the bookkeeping
     * directory, see bookkeepingPaths().
     */
    inline void makeDeadBookkeepingInstanceDir(
        fs::path const& root,
        std::string const& instanceId,
        std::optional<std::chrono::system_clock::time_point> deadAt = std::nullopt
    )
    {
        const auto paths = bookkeepingPaths(root / instanceId);
        fs::create_directories(paths.lock.parent_path());
        std::ofstream{paths.lock}.flush();
        writeTestMetadata(paths.metadata, instanceId, deadAt);
    }

    /**
     * @brief Simulate the on-disk remnants of a dead TemporaryDirectoryInstance of a build without the bookkeeping
     *        directory.
     *
     * Creates @p root / @p instanceId / with:
     *  - @c \<instanceId\>.lock — an unowned plain file (isLockedByAnother returns false)
     *  - @c metadata.json — with the given @p deadAt (or null if absent)
     *
     * @param root        Parent directory (the tempRootDir the manager watches).
     * @param instanceId  Name used for the subdirectory and the lock file.
     * @param deadAt      Optional death timestamp written into metadata.json.
     */
    inline void makeDeadInstanceDir(
        fs::path const& root,
        std::string const& instanceId,
        std::optional<std::chrono::system_clock::time_point> deadAt = std::nullopt
    )
    {
        auto dir = root / instanceId;
        fs::create_directories(dir);

        // Lock file exists but is not held by any process
        std::ofstream{dir / (instanceId + ".lock")}.flush();
        writeTestMetadata(dir / "metadata.json", instanceId, deadAt);
    }

    // =========================================================================
    // InstanceLockTest — no display required
    // =========================================================================

    class InstanceLockTest : public ::testing::Test
    {
      protected:
        fs::path root_;

        void SetUp() override
        {
            root_ = makeTestTempRoot("lock");
        }
        void TearDown() override
        {
            fs::remove_all(root_);
        }
    };

    TEST_F(InstanceLockTest, ConstructorCreatesLockFile)
    {
        auto lockPath = root_ / "a.lock";
        InstanceLock lock{lockPath};
        EXPECT_TRUE(fs::exists(lockPath));
    }

    TEST_F(InstanceLockTest, HasLockReturnsTrueAfterConstruction)
    {
        InstanceLock lock{root_ / "b.lock"};
        EXPECT_TRUE(lock.hasLock());
    }

    TEST_F(InstanceLockTest, IsLockedByAnotherReturnsFalseForUnownedFile)
    {
        auto lockPath = root_ / "c.lock";
        std::ofstream{lockPath}.flush(); // file exists, nobody locks it
        EXPECT_FALSE(InstanceLock::isLockedByAnother(lockPath));
    }

    TEST_F(InstanceLockTest, IsLockedByAnotherReturnsTrueWhileLockHeld)
    {
        auto lockPath = root_ / "d.lock";
        InstanceLock lock{lockPath};
        // Same process, different open-file-description: flock(LOCK_NB) is denied
        EXPECT_TRUE(InstanceLock::isLockedByAnother(lockPath));
    }

    TEST_F(InstanceLockTest, IsLockedByAnotherReturnsFalseAfterLockDestroyed)
    {
        auto lockPath = root_ / "e.lock";
        {
            InstanceLock lock{lockPath};
            ASSERT_TRUE(InstanceLock::isLockedByAnother(lockPath));
        }
        EXPECT_FALSE(InstanceLock::isLockedByAnother(lockPath));
    }

    TEST_F(InstanceLockTest, MoveConstructorTransfersOwnership)
    {
        auto lockPath = root_ / "f.lock";
        InstanceLock src{lockPath};
        InstanceLock dst{std::move(src)};
        EXPECT_FALSE(src.hasLock());
        EXPECT_TRUE(dst.hasLock());
    }

    TEST_F(InstanceLockTest, MovedLockStillBlocksIsLockedByAnother)
    {
        auto lockPath = root_ / "g.lock";
        InstanceLock src{lockPath};
        InstanceLock dst{std::move(src)};
        EXPECT_TRUE(InstanceLock::isLockedByAnother(lockPath));
    }

    TEST_F(InstanceLockTest, TwoDistinctFilesLockIndependently)
    {
        auto p1 = root_ / "h1.lock";
        auto p2 = root_ / "h2.lock";
        InstanceLock l1{p1};
        InstanceLock l2{p2};
        EXPECT_TRUE(l1.hasLock());
        EXPECT_TRUE(l2.hasLock());
        EXPECT_TRUE(InstanceLock::isLockedByAnother(p1));
        EXPECT_TRUE(InstanceLock::isLockedByAnother(p2));
    }

    TEST_F(InstanceLockTest, IsLockedByAnotherReturnsFalseForNonexistentFile)
    {
        auto lockPath = root_ / "nonexistent.lock";
        // File doesn't exist yet; isLockedByAnother creates it and checks
        // (it creates the file with O_CREAT, so the file will exist after)
        bool result = InstanceLock::isLockedByAnother(lockPath);
        EXPECT_FALSE(result);
    }

    // =========================================================================
    // InstanceWatchTest — no display required (tests the move-only RAII type)
    // =========================================================================

    class InstanceWatchTest : public ::testing::Test
    {};

    TEST_F(InstanceWatchTest, DefaultConstructedIsInvalid)
    {
        InstanceWatch watch;
        EXPECT_FALSE(watch.isValid());
    }

    TEST_F(InstanceWatchTest, DefaultConstructedHasNegativeWatchId)
    {
        InstanceWatch watch;
        EXPECT_LT(watch.watchId(), 0);
    }

    TEST_F(InstanceWatchTest, DefaultConstructedHasEmptyPath)
    {
        InstanceWatch watch;
        EXPECT_TRUE(watch.path().empty());
    }

    TEST_F(InstanceWatchTest, ReleaseOnDefaultConstructedIsNoOp)
    {
        InstanceWatch watch;
        EXPECT_NO_THROW(watch.release());
        EXPECT_FALSE(watch.isValid());
    }

    TEST_F(InstanceWatchTest, MoveConstructFromDefaultResultsInBothInvalid)
    {
        InstanceWatch src;
        InstanceWatch dst{std::move(src)};
        EXPECT_FALSE(src.isValid());
        EXPECT_FALSE(dst.isValid());
    }

    // =========================================================================
    // TemporaryDirectoryInstanceTest
    // =========================================================================

    class TemporaryDirectoryInstanceTest : public ::testing::Test
    {
      protected:
        boost::asio::io_context ioc_;
        fs::path root_;

        void SetUp() override
        {
            root_ = makeTestTempRoot("tdi");
        }

        void TearDown() override
        {
            if (!root_.empty())
                fs::remove_all(root_);
        }

        auto makeStrand()
        {
            return boost::asio::make_strand(ioc_.get_executor());
        }

        std::unique_ptr<TemporaryDirectoryInstance> makeInstance()
        {
            return std::make_unique<TemporaryDirectoryInstance>(
                TemporaryDirectoryInstance::Config{.tempRootDir = root_}, makeStrand()
            );
        }
    };

    TEST_F(TemporaryDirectoryInstanceTest, InstanceDirCreatedOnConstruction)
    {
        auto inst = makeInstance();
        EXPECT_TRUE(fs::exists(inst->instanceDir()));
        EXPECT_TRUE(fs::is_directory(inst->instanceDir()));
    }

    TEST_F(TemporaryDirectoryInstanceTest, InstanceDirIsDirectChildOfTempRoot)
    {
        auto inst = makeInstance();
        auto rel = fs::relative(inst->instanceDir(), root_);
        // Exactly one component with no ".."
        EXPECT_FALSE(rel.empty());
        EXPECT_FALSE(rel.string().starts_with(".."));
        EXPECT_EQ(std::distance(rel.begin(), rel.end()), 1);
    }

    TEST_F(TemporaryDirectoryInstanceTest, InstanceIdMatchesDirectoryName)
    {
        auto inst = makeInstance();
        EXPECT_EQ(inst->instanceDir().filename().string(), inst->instanceId());
    }

    TEST_F(TemporaryDirectoryInstanceTest, InstanceIdIsNonEmpty)
    {
        auto inst = makeInstance();
        EXPECT_FALSE(inst->instanceId().empty());
    }

    TEST_F(TemporaryDirectoryInstanceTest, LockFileExistsInBookkeepingDir)
    {
        auto inst = makeInstance();
        EXPECT_TRUE(fs::exists(bookkeepingPaths(inst->instanceDir()).lock));
        EXPECT_FALSE(fs::exists(legacyBookkeepingPaths(inst->instanceDir()).lock));
    }

    TEST_F(TemporaryDirectoryInstanceTest, BookkeepingDirIsTheOnlyEntryOfANewInstance)
    {
        auto inst = makeInstance();
        std::vector<fs::path> entries;
        for (auto const& entry : fs::directory_iterator{inst->instanceDir()})
            entries.push_back(entry.path().filename());
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries.front(), fs::path{bookkeepingDirectoryName});
    }

    TEST_F(TemporaryDirectoryInstanceTest, ContentNamedLikeLegacyBookkeepingIsLeftAlone)
    {
        fs::path dir;
        {
            auto inst = makeInstance();
            dir = inst->instanceDir();
            std::ofstream{dir / "metadata.json"} << "tracked content";
            std::ofstream{dir / (inst->instanceId() + ".lock")} << "tracked lock content";
        }
        EXPECT_EQ(readText(dir / "metadata.json"), "tracked content");
        EXPECT_EQ(readText(dir / (dir.filename().string() + ".lock")), "tracked lock content");
        const auto meta = readJson(bookkeepingPaths(dir).metadata);
        EXPECT_FALSE(meta["deadAt"].is_null());
    }

    TEST_F(TemporaryDirectoryInstanceTest, LockFileIsHeldDuringLifetime)
    {
        auto inst = makeInstance();
        auto lockPath = bookkeepingPaths(inst->instanceDir()).lock;
        EXPECT_TRUE(InstanceLock::isLockedByAnother(lockPath));
    }

    TEST_F(TemporaryDirectoryInstanceTest, LockReleasedAfterDestruction)
    {
        fs::path lockPath;
        fs::path dir;
        {
            auto inst = makeInstance();
            dir = inst->instanceDir();
            lockPath = bookkeepingPaths(dir).lock;
            ASSERT_TRUE(InstanceLock::isLockedByAnother(lockPath));
        }
        EXPECT_FALSE(InstanceLock::isLockedByAnother(lockPath));
        fs::remove_all(dir);
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataFileCreatedOnConstruction)
    {
        auto inst = makeInstance();
        EXPECT_TRUE(fs::exists(bookkeepingPaths(inst->instanceDir()).metadata));
        EXPECT_FALSE(fs::exists(inst->instanceDir() / "metadata.json"));
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataContainsCorrectInstanceId)
    {
        auto inst = makeInstance();
        auto meta = readJson(bookkeepingPaths(inst->instanceDir()).metadata);
        EXPECT_EQ(meta.at("instanceId").get<std::string>(), inst->instanceId());
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataHasCreatedAt)
    {
        auto inst = makeInstance();
        auto meta = readJson(bookkeepingPaths(inst->instanceDir()).metadata);
        ASSERT_TRUE(meta.contains("createdAt"));
        EXPECT_FALSE(meta["createdAt"].is_null());
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataHasNullDeadAtInitially)
    {
        auto inst = makeInstance();
        auto meta = readJson(bookkeepingPaths(inst->instanceDir()).metadata);
        ASSERT_TRUE(meta.contains("deadAt"));
        EXPECT_TRUE(meta["deadAt"].is_null());
    }

    TEST_F(TemporaryDirectoryInstanceTest, IsValidTrueOnConstruction)
    {
        auto inst = makeInstance();
        EXPECT_TRUE(inst->isValid());
    }

    TEST_F(TemporaryDirectoryInstanceTest, DirectoryNotDeletedOnDestruction)
    {
        fs::path dir;
        {
            auto inst = makeInstance();
            dir = inst->instanceDir();
        }
        EXPECT_TRUE(fs::exists(dir));
        fs::remove_all(dir);
    }

    TEST_F(TemporaryDirectoryInstanceTest, DeadTimestampWrittenOnDestruction)
    {
        fs::path metaPath;
        {
            auto inst = makeInstance();
            metaPath = bookkeepingPaths(inst->instanceDir()).metadata;
        }
        ASSERT_TRUE(fs::exists(metaPath));
        auto const meta = readJson(metaPath);
        ASSERT_TRUE(meta.contains("deadAt"));
        EXPECT_FALSE(meta["deadAt"].is_null());
    }

    TEST_F(TemporaryDirectoryInstanceTest, CleanupNowDeletesDirectory)
    {
        auto inst = makeInstance();
        auto dir = inst->instanceDir();
        inst->cleanupNow();
        EXPECT_FALSE(fs::exists(dir));
    }

    TEST_F(TemporaryDirectoryInstanceTest, IsValidFalseAfterCleanupNow)
    {
        auto inst = makeInstance();
        inst->cleanupNow();
        EXPECT_FALSE(inst->isValid());
    }

    TEST_F(TemporaryDirectoryInstanceTest, MultipleInstancesHaveDistinctIds)
    {
        auto alpha = makeInstance();
        auto beta = makeInstance();
        EXPECT_NE(alpha->instanceId(), beta->instanceId());
    }

    TEST_F(TemporaryDirectoryInstanceTest, MultipleInstancesHaveDistinctDirs)
    {
        auto alpha = makeInstance();
        auto beta = makeInstance();
        EXPECT_NE(alpha->instanceDir(), beta->instanceDir());
    }

    TEST_F(TemporaryDirectoryInstanceTest, AddWatchNullOptForNonexistentPath)
    {
        auto inst = makeInstance();
        auto result = inst->addWatch(inst->instanceDir() / "does_not_exist");
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(TemporaryDirectoryInstanceTest, AddWatchNullOptForPathOutsideInstanceDir)
    {
        auto inst = makeInstance();
        // root_ is the parent of instanceDir, so it is outside
        auto result = inst->addWatch(root_);
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(TemporaryDirectoryInstanceTest, AddWatchReturnsValidHandleForSubdir)
    {
        auto inst = makeInstance();
        auto subdir = inst->instanceDir() / "watched";
        fs::create_directory(subdir);

        auto watch = inst->addWatch(subdir);
        ASSERT_TRUE(watch.has_value());
        EXPECT_TRUE(watch->isValid());
        EXPECT_EQ(watch->path(), subdir);
    }

    TEST_F(TemporaryDirectoryInstanceTest, AddWatchReturnsValidHandleForANonAsciiSubdir)
    {
        auto inst = makeInstance();
        auto subdir = inst->instanceDir() / Paths::nativeFromUtf8("Größe 東京");
        fs::create_directory(subdir);

        auto watch = inst->addWatch(subdir);
        ASSERT_TRUE(watch.has_value());
        EXPECT_TRUE(watch->isValid());
        EXPECT_EQ(watch->path(), subdir);
    }

    TEST_F(TemporaryDirectoryInstanceTest, AddWatchReturnsValidHandleForInstanceDirItself)
    {
        auto inst = makeInstance();
        auto watch = inst->addWatch(inst->instanceDir());
        ASSERT_TRUE(watch.has_value());
        EXPECT_TRUE(watch->isValid());
    }

    TEST_F(TemporaryDirectoryInstanceTest, WatchIsInvalidAfterRelease)
    {
        auto inst = makeInstance();
        auto subdir = inst->instanceDir() / "rel";
        fs::create_directory(subdir);

        auto watch = inst->addWatch(subdir);
        ASSERT_TRUE(watch.has_value());
        watch->release();
        EXPECT_FALSE(watch->isValid());
    }

    TEST_F(TemporaryDirectoryInstanceTest, WatchOutlivingItsInstanceReleasesSafely)
    {
        auto inst = makeInstance();
        auto watch = inst->addWatch(inst->instanceDir());
        ASSERT_TRUE(watch.has_value());
        inst.reset();
        EXPECT_NO_THROW(watch->release());
        EXPECT_FALSE(watch->isValid());
    }

    TEST_F(TemporaryDirectoryInstanceTest, WatchAfterCleanupNowReleasesSafely)
    {
        auto inst = makeInstance();
        auto watch = inst->addWatch(inst->instanceDir());
        ASSERT_TRUE(watch.has_value());
        inst->cleanupNow();
        EXPECT_NO_THROW(watch->release());
    }

    TEST_F(TemporaryDirectoryInstanceTest, MoveFromWatchInvalidatesSource)
    {
        auto inst = makeInstance();
        auto subdir = inst->instanceDir() / "mv";
        fs::create_directory(subdir);

        auto watch = inst->addWatch(subdir);
        ASSERT_TRUE(watch.has_value());
        InstanceWatch moved{std::move(*watch)};
        EXPECT_FALSE(watch->isValid());
        EXPECT_TRUE(moved.isValid());
    }

    // ---- recordOrigin behaviour ----

    TEST_F(TemporaryDirectoryInstanceTest, RecordedOriginKeepsTheLegacyFields)
    {
        auto inst = makeInstance();
        const auto metaPath = bookkeepingPaths(inst->instanceDir()).metadata;
        const auto before = readJson(metaPath);

        const auto recorded = inst->recordOrigin(remotePathOf("/srv/data/report.txt"));

        ASSERT_TRUE(recorded.has_value()) << recorded.error();
        const auto after = readJson(metaPath);
        EXPECT_EQ(after.at("instanceId"), inst->instanceId());
        EXPECT_EQ(after.at("createdAt"), before.at("createdAt"));
        ASSERT_TRUE(after.contains("deadAt"));
        EXPECT_TRUE(after.at("deadAt").is_null());
        EXPECT_EQ(after.at("remotePath"), "/srv/data/report.txt");
        const auto read = readMetadata(inst->instanceDir());
        ASSERT_TRUE(read.has_value()) << read.error();
        ASSERT_TRUE(read->has_value());
        EXPECT_EQ(**read, after);
    }

    TEST_F(TemporaryDirectoryInstanceTest, RecordedOriginSurvivesTheDeathTimestamp)
    {
        fs::path metaPath;
        nlohmann::json before;
        {
            auto inst = makeInstance();
            metaPath = bookkeepingPaths(inst->instanceDir()).metadata;
            ASSERT_TRUE(inst->recordOrigin(remotePathOf("/srv/data/report.txt")).has_value());
            before = readJson(metaPath);
        }
        const auto after = readJson(metaPath);
        EXPECT_EQ(after.at("instanceId"), before.at("instanceId"));
        EXPECT_EQ(after.at("createdAt"), before.at("createdAt"));
        EXPECT_TRUE(after.at("deadAt").is_number_integer());
        EXPECT_EQ(after.at("remotePath"), "/srv/data/report.txt");
    }

    TEST_F(TemporaryDirectoryInstanceTest, RecordingTheOriginAgainReplacesIt)
    {
        auto inst = makeInstance();
        ASSERT_TRUE(inst->recordOrigin(remotePathOf("/srv/first.txt")).has_value());
        ASSERT_TRUE(inst->recordOrigin(remotePathOf("/srv/second.txt")).has_value());
        EXPECT_EQ(readJson(bookkeepingPaths(inst->instanceDir()).metadata)["remotePath"], "/srv/second.txt");
    }

    TEST_F(TemporaryDirectoryInstanceTest, RecordingTheOriginAfterCleanupNowFails)
    {
        auto inst = makeInstance();
        const auto dir = inst->instanceDir();
        inst->cleanupNow();

        const auto recorded = inst->recordOrigin(remotePathOf("/srv/data/report.txt"));

        ASSERT_FALSE(recorded.has_value());
        EXPECT_NE(recorded.error().find(inst->instanceId()), std::string::npos) << recorded.error();
        EXPECT_FALSE(fs::exists(fs::symlink_status(dir)));
    }

    // =========================================================================
    // TempDirInstanceManagerTest
    // =========================================================================

    class TempDirInstanceManagerTest : public ::testing::Test
    {
      protected:
        boost::asio::io_context ioc_;
        fs::path root_;

        void SetUp() override
        {
            root_ = makeTestTempRoot("mgr");
        }

        void TearDown() override
        {
            if (!root_.empty())
                fs::remove_all(root_);
        }

        std::unique_ptr<TempDirInstanceManager> makeManager(std::chrono::hours retention = 24h)
        {
            return std::make_unique<TempDirInstanceManager>(
                boost::asio::make_strand(ioc_.get_executor()), root_, retention
            );
        }
    };

    TEST_F(TempDirInstanceManagerTest, CreateInstanceReturnsNonNull)
    {
        auto mgr = makeManager();
        EXPECT_NE(mgr->createInstance(), nullptr);
    }

    TEST_F(TempDirInstanceManagerTest, FindInstanceReturnsPointerAfterCreate)
    {
        auto mgr = makeManager();
        auto* ptr = mgr->createInstance();
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(mgr->findInstance(ptr->instanceId()), ptr);
    }

    TEST_F(TempDirInstanceManagerTest, FindNonexistentInstanceReturnsNull)
    {
        auto mgr = makeManager();
        EXPECT_EQ(mgr->findInstance("nonexistent-id-xyz"), nullptr);
    }

    TEST_F(TempDirInstanceManagerTest, ListInstancesContainsCreated)
    {
        auto mgr = makeManager();
        auto* ptr = mgr->createInstance();
        ASSERT_NE(ptr, nullptr);
        auto list = mgr->listInstances();
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].instanceId, ptr->instanceId());
        EXPECT_EQ(list[0].instanceDir, ptr->instanceDir());
    }

    TEST_F(TempDirInstanceManagerTest, ListInstanceIdsContainsCreatedId)
    {
        auto mgr = makeManager();
        auto* ptr = mgr->createInstance();
        ASSERT_NE(ptr, nullptr);
        auto ids = mgr->listInstanceIds();
        ASSERT_EQ(ids.size(), 1u);
        EXPECT_EQ(ids[0], ptr->instanceId());
    }

    TEST_F(TempDirInstanceManagerTest, ListIsEmptyInitially)
    {
        auto mgr = makeManager();
        EXPECT_TRUE(mgr->listInstances().empty());
        EXPECT_TRUE(mgr->listInstanceIds().empty());
    }

    TEST_F(TempDirInstanceManagerTest, DestroyInstanceRemovesFromList)
    {
        auto mgr = makeManager();
        auto* ptr = mgr->createInstance();
        ASSERT_NE(ptr, nullptr);
        std::string id = ptr->instanceId();
        mgr->destroyInstance(id);
        EXPECT_EQ(mgr->findInstance(id), nullptr);
        EXPECT_TRUE(mgr->listInstances().empty());
    }

    TEST_F(TempDirInstanceManagerTest, DestroyNonexistentIsNoOp)
    {
        auto mgr = makeManager();
        EXPECT_NO_THROW(mgr->destroyInstance("nonexistent-id-xyz"));
    }

    TEST_F(TempDirInstanceManagerTest, CreateMultipleInstancesAllTracked)
    {
        auto mgr = makeManager();
        auto* alpha = mgr->createInstance();
        auto* beta = mgr->createInstance();
        auto* gamma = mgr->createInstance();
        ASSERT_NE(alpha, nullptr);
        ASSERT_NE(beta, nullptr);
        ASSERT_NE(gamma, nullptr);
        EXPECT_EQ(mgr->listInstances().size(), 3u);
    }

    TEST_F(TempDirInstanceManagerTest, AllCreatedInstancesHaveDistinctIds)
    {
        auto mgr = makeManager();
        auto* alpha = mgr->createInstance();
        auto* beta = mgr->createInstance();
        ASSERT_NE(alpha, nullptr);
        ASSERT_NE(beta, nullptr);
        EXPECT_NE(alpha->instanceId(), beta->instanceId());
    }

    TEST_F(TempDirInstanceManagerTest, DestroyOneOfMultipleKeepsRemainder)
    {
        auto mgr = makeManager();
        auto* alpha = mgr->createInstance();
        auto* beta = mgr->createInstance();
        ASSERT_NE(alpha, nullptr);
        ASSERT_NE(beta, nullptr);
        std::string keepId = beta->instanceId();
        mgr->destroyInstance(alpha->instanceId());
        EXPECT_EQ(mgr->listInstances().size(), 1u);
        EXPECT_NE(mgr->findInstance(keepId), nullptr);
    }

    TEST_F(TempDirInstanceManagerTest, CreatedInstanceDirIsUnderTempRoot)
    {
        auto mgr = makeManager();
        auto* ptr = mgr->createInstance();
        ASSERT_NE(ptr, nullptr);
        auto rel = fs::relative(ptr->instanceDir(), root_);
        EXPECT_FALSE(rel.string().starts_with(".."));
    }

    // ---- manualCleanup behaviour ----

    TEST_F(TempDirInstanceManagerTest, ManualCleanupDoesNotDeleteLiveInstances)
    {
        // Even with 0h retention, live instances must be skipped by name
        auto mgr = makeManager(0h);
        auto* ptr = mgr->createInstance();
        ASSERT_NE(ptr, nullptr);
        auto dir = ptr->instanceDir();
        mgr->manualCleanup();
        EXPECT_TRUE(fs::exists(dir));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupIgnoresDirWithoutLockFile)
    {
        auto mgr = makeManager(0h);
        auto noLockDir = root_ / "no_lock_dir";
        fs::create_directories(noLockDir);
        mgr->manualCleanup();
        EXPECT_TRUE(fs::exists(noLockDir));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupWritesDeadAtWhenMetadataMissing)
    {
        auto mgr = makeManager(24h);
        std::string deadId = "dead-no-meta";
        auto dir = root_ / deadId;
        fs::create_directories(dir);
        std::ofstream{dir / (deadId + ".lock")}.flush(); // no metadata.json

        mgr->manualCleanup();

        // Added in the bookkeeping directory, no legacy metadata appears.
        auto metaPath = bookkeepingPaths(dir).metadata;
        ASSERT_TRUE(fs::exists(metaPath));
        EXPECT_FALSE(readJson(metaPath)["deadAt"].is_null());
        EXPECT_FALSE(fs::exists(dir / "metadata.json"));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupWritesDeadAtWhenDeadAtIsNull)
    {
        auto mgr = makeManager(24h);
        std::string deadId = "dead-null-deadat";
        makeDeadInstanceDir(root_, deadId); // deadAt = null

        mgr->manualCleanup();

        // Migrated by adding the bookkeeping metadata, the legacy file gets the same stamp.
        const auto added = readJson(bookkeepingPaths(root_ / deadId).metadata);
        const auto legacy = readJson(root_ / deadId / "metadata.json");
        EXPECT_FALSE(added["deadAt"].is_null());
        EXPECT_EQ(added["deadAt"], legacy["deadAt"]);
        EXPECT_EQ(added["instanceId"], deadId);
        EXPECT_TRUE(fs::exists(root_ / deadId / (deadId + ".lock")));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupDeletesExpiredBookkeepingInstance)
    {
        auto mgr = makeManager(1h);
        makeDeadBookkeepingInstanceDir(root_, "dead-new-expired", std::chrono::system_clock::now() - 2h);
        makeDeadBookkeepingInstanceDir(root_, "dead-new-recent", std::chrono::system_clock::now() - 10min);

        mgr->manualCleanup();

        EXPECT_FALSE(fs::exists(root_ / "dead-new-expired"));
        EXPECT_TRUE(fs::exists(root_ / "dead-new-recent"));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupKeepsInstanceWhoseBookkeepingLockIsHeld)
    {
        auto mgr = makeManager(0h);
        makeDeadBookkeepingInstanceDir(root_, "held-elsewhere", std::chrono::system_clock::now() - 2h);
        InstanceLock heldLock{bookkeepingPaths(root_ / "held-elsewhere").lock};

        mgr->manualCleanup();

        EXPECT_TRUE(fs::exists(root_ / "held-elsewhere"));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupKeepsLegacyInstanceWhoseLockIsHeld)
    {
        auto mgr = makeManager(0h);
        makeDeadInstanceDir(root_, "legacy-held", std::chrono::system_clock::now() - 2h);
        InstanceLock heldLock{root_ / "legacy-held" / "legacy-held.lock"};

        mgr->manualCleanup();

        EXPECT_TRUE(fs::exists(root_ / "legacy-held"));
        EXPECT_FALSE(fs::exists(root_ / "legacy-held" / fs::path{bookkeepingDirectoryName}));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupDeletesExpiredDeadInstance)
    {
        auto mgr = makeManager(1h);
        std::string deadId = "dead-expired";
        makeDeadInstanceDir(root_, deadId, std::chrono::system_clock::now() - 2h);

        mgr->manualCleanup();

        EXPECT_FALSE(fs::exists(root_ / deadId));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupPreservesRecentDeadInstance)
    {
        auto mgr = makeManager(24h);
        std::string deadId = "dead-recent";
        makeDeadInstanceDir(root_, deadId, std::chrono::system_clock::now() - 1h);

        mgr->manualCleanup();

        // 1 hour elapsed < 24h retention → must survive
        EXPECT_TRUE(fs::exists(root_ / deadId));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupWithZeroRetentionDeletesImmediately)
    {
        // 0h retention: any elapsed >= 0h triggers deletion
        auto mgr = makeManager(0h);
        std::string deadId = "dead-zero-ret";
        // deadAt was set 1 minute ago: duration_cast<hours>(1min) = 0h >= 0h → delete
        makeDeadInstanceDir(root_, deadId, std::chrono::system_clock::now() - 1min);

        mgr->manualCleanup();

        EXPECT_FALSE(fs::exists(root_ / deadId));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupHandlesMixedExpiredAndRecent)
    {
        auto mgr = makeManager(1h);
        makeDeadInstanceDir(root_, "dead-old-a", std::chrono::system_clock::now() - 3h);
        makeDeadInstanceDir(root_, "dead-old-b", std::chrono::system_clock::now() - 5h);
        makeDeadInstanceDir(root_, "dead-new-a", std::chrono::system_clock::now() - 30min);

        mgr->manualCleanup();

        EXPECT_FALSE(fs::exists(root_ / "dead-old-a"));
        EXPECT_FALSE(fs::exists(root_ / "dead-old-b"));
        EXPECT_TRUE(fs::exists(root_ / "dead-new-a"));
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupDeadInstanceAfterDestroyInManager)
    {
        // createInstance → destroyInstance releases the lock and writes deadAt.
        // With 0h retention a subsequent manualCleanup should delete the dir.
        auto mgr = makeManager(0h);
        auto* ptr = mgr->createInstance();
        ASSERT_NE(ptr, nullptr);
        auto dir = ptr->instanceDir();
        std::string id = ptr->instanceId();

        // Destroy removes from map and runs ~TemporaryDirectoryInstance
        // (lock released, deadAt written)
        mgr->destroyInstance(id);

        // deadAt = now, elapsed = 0h, retention = 0h  →  0h >= 0h → delete
        mgr->manualCleanup();

        EXPECT_FALSE(fs::exists(dir));
    }

    TEST_F(TempDirInstanceManagerTest, StartupCleanupRunsOnTheStrand)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "dead-before-start", std::chrono::system_clock::now() - 25h);

        mgr->scheduleStartupCleanup();
        EXPECT_TRUE(fs::exists(root_ / "dead-before-start"));

        ioc_.poll();
        EXPECT_FALSE(fs::exists(root_ / "dead-before-start"));
    }

    TEST_F(TempDirInstanceManagerTest, StartupCleanupRemovesOnlyInstancesDeadForTheRetention)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "dead-long-ago", std::chrono::system_clock::now() - 25h);
        makeDeadInstanceDir(root_, "legacy-dead-long-ago", std::chrono::system_clock::now() - 48h);
        makeDeadBookkeepingInstanceDir(root_, "dead-recently", std::chrono::system_clock::now() - 23h);

        mgr->scheduleStartupCleanup();
        ioc_.poll();

        EXPECT_FALSE(fs::exists(root_ / "dead-long-ago"));
        EXPECT_FALSE(fs::exists(root_ / "legacy-dead-long-ago"));
        EXPECT_TRUE(fs::exists(root_ / "dead-recently"));
    }

    TEST_F(TempDirInstanceManagerTest, StartupCleanupStampsAnUnstampedInstance)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "never-stamped");
        const auto before = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        mgr->scheduleStartupCleanup();
        ioc_.poll();

        ASSERT_TRUE(fs::exists(root_ / "never-stamped"));
        const auto deadAt = readJson(bookkeepingPaths(root_ / "never-stamped").metadata)["deadAt"];
        ASSERT_TRUE(deadAt.is_number_integer());
        EXPECT_GE(deadAt.get<std::int64_t>(), static_cast<std::int64_t>(before));
    }

    TEST_F(TempDirInstanceManagerTest, StartupCleanupLeavesLiveInstancesAlone)
    {
        auto mgr = makeManager(0h);
        auto* live = mgr->createInstance();
        ASSERT_NE(live, nullptr);
        std::ofstream{live->instanceDir() / "content.txt"} << "content";
        makeDeadBookkeepingInstanceDir(root_, "dead-long-ago", std::chrono::system_clock::now() - 25h);

        mgr->scheduleStartupCleanup();
        ioc_.poll();

        EXPECT_FALSE(fs::exists(root_ / "dead-long-ago"));
        EXPECT_TRUE(fs::exists(live->instanceDir() / "content.txt"));
        EXPECT_TRUE(readJson(bookkeepingPaths(live->instanceDir()).metadata)["deadAt"].is_null());
        EXPECT_EQ(mgr->findInstance(live->instanceId()), live);
    }

    // ---- discardContent behaviour ----

    TEST_F(TempDirInstanceManagerTest, DiscardingTheOnlyContentRemovesTheInstanceAndItsDirectory)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        auto* other = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        ASSERT_NE(other, nullptr);
        const auto directory = instance->instanceDir();
        const auto id = instance->instanceId();
        std::ofstream{directory / "script.sh"} << "#!/bin/sh";
        ASSERT_TRUE(instance->addWatch(directory).has_value());
        makeDeadBookkeepingInstanceDir(root_, "dead-elsewhere", std::chrono::system_clock::now() - 1h);

        const auto outcome = mgr->discardContent(id, directory / "script.sh");

        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_EQ(*outcome, DiscardOutcome::InstanceDestroyed);
        EXPECT_FALSE(fs::exists(fs::symlink_status(directory)));
        EXPECT_EQ(mgr->findInstance(id), nullptr);
        EXPECT_TRUE(fs::exists(other->instanceDir()));
        EXPECT_TRUE(fs::exists(root_ / "dead-elsewhere"));

        mgr->manualCleanup();
        EXPECT_FALSE(fs::exists(fs::symlink_status(directory)));
    }

    TEST_F(TempDirInstanceManagerTest, DestroyingKeepsTheDeadInstanceDirectoryForTheRetention)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        std::ofstream{directory / "content.txt"} << "content";

        mgr->destroyInstance(instance->instanceId());
        mgr->manualCleanup();

        EXPECT_TRUE(fs::exists(directory / "content.txt"));
        EXPECT_FALSE(readJson(bookkeepingPaths(directory).metadata)["deadAt"].is_null());
    }

    TEST_F(TempDirInstanceManagerTest, DiscardingKeepsAnInstanceWithOtherContent)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        std::ofstream{directory / "discarded.sh"} << "#!/bin/sh";
        std::ofstream{directory / "kept.txt"} << "kept";

        const auto outcome = mgr->discardContent(instance->instanceId(), directory / "discarded.sh");

        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_EQ(*outcome, DiscardOutcome::InstanceKept);
        EXPECT_FALSE(fs::exists(directory / "discarded.sh"));
        EXPECT_TRUE(fs::exists(directory / "kept.txt"));
        EXPECT_EQ(mgr->findInstance(instance->instanceId()), instance);
    }

    TEST_F(TempDirInstanceManagerTest, DiscardingADirectoryRemovesItWhole)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        fs::create_directories(directory / "tree" / "below");
        std::ofstream{directory / "tree" / "below" / "file.txt"} << "content";

        const auto outcome = mgr->discardContent(instance->instanceId(), directory / "tree");

        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_EQ(*outcome, DiscardOutcome::InstanceDestroyed);
        EXPECT_FALSE(fs::exists(directory));
    }

    TEST_F(TempDirInstanceManagerTest, DiscardingALinkKeepsItsTarget)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        const auto outside = root_ / "outside";
        fs::create_directories(outside);
        std::ofstream{outside / "target.txt"} << "target";
        std::error_code linkError;
        fs::create_directory_symlink(outside, directory / "link", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create symlinks here: " << linkError.message();

        const auto outcome = mgr->discardContent(instance->instanceId(), directory / "link");

        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_FALSE(fs::exists(fs::symlink_status(directory / "link")));
        EXPECT_TRUE(fs::exists(outside / "target.txt"));
    }

    TEST_F(TempDirInstanceManagerTest, DiscardingRefusesPathsThatAreNoContent)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        const auto id = instance->instanceId();
        const auto outside = root_ / "outside";
        fs::create_directories(outside);
        std::ofstream{outside / "target.txt"} << "target";
        std::ofstream{directory / "content.txt"} << "content";

        EXPECT_FALSE(mgr->discardContent(id, directory).has_value());
        EXPECT_FALSE(mgr->discardContent(id, directory / ".").has_value());
        EXPECT_FALSE(mgr->discardContent(id, directory / "..").has_value());
        EXPECT_FALSE(mgr->discardContent(id, directory / ".." / "outside" / "target.txt").has_value());
        EXPECT_FALSE(mgr->discardContent(id, outside / "target.txt").has_value());
        EXPECT_FALSE(mgr->discardContent(id, directory / fs::path{bookkeepingDirectoryName}).has_value());
        EXPECT_FALSE(mgr->discardContent(id, bookkeepingPaths(directory).metadata).has_value());
        EXPECT_FALSE(mgr->discardContent("nonexistent-id-xyz", directory / "content.txt").has_value());

        std::error_code linkError;
        fs::create_directory_symlink(outside, directory / "link", linkError);
        if (!linkError)
            EXPECT_FALSE(mgr->discardContent(id, directory / "link" / "target.txt").has_value());

        EXPECT_TRUE(fs::exists(outside / "target.txt"));
        EXPECT_TRUE(fs::exists(directory / "content.txt"));
        EXPECT_TRUE(fs::exists(bookkeepingPaths(directory).metadata));
        EXPECT_EQ(mgr->findInstance(id), instance);
    }

    TEST_F(TempDirInstanceManagerTest, MultipleCleanupRunsAreIdempotent)
    {
        auto mgr = makeManager(1h);
        std::string deadId = "dead-idempotent";
        makeDeadInstanceDir(root_, deadId, std::chrono::system_clock::now() - 2h);

        mgr->manualCleanup();
        ASSERT_FALSE(fs::exists(root_ / deadId));

        // Second call must not throw even though the dir is already gone
        EXPECT_NO_THROW(mgr->manualCleanup());
    }

    // ---- listOrphaned behaviour ----

    TEST_F(TempDirInstanceManagerTest, ListingOrphansDescribesAnEndedCopy)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        const auto id = instance->instanceId();
        writeFileOfSize(directory / "notes.txt", 5u);
        ASSERT_TRUE(instance->recordOrigin(remotePathOf("/srv/notes.txt")).has_value());
        mgr->destroyInstance(id);
        const auto meta = readJson(bookkeepingPaths(directory).metadata);

        const auto copies = mgr->listOrphaned();

        ASSERT_EQ(copies.size(), 1u);
        auto const& copy = copies.front();
        EXPECT_EQ(copy.instanceId.value(), id);
        EXPECT_EQ(copy.name, "notes.txt");
        EXPECT_EQ(copy.directory.native(), fs::weakly_canonical(directory));
        ASSERT_TRUE(copy.copyPath.has_value());
        EXPECT_EQ(copy.copyPath->native(), fs::weakly_canonical(directory) / "notes.txt");
        ASSERT_TRUE(copy.remotePath.has_value());
        EXPECT_EQ(copy.remotePath->value(), "/srv/notes.txt");
        EXPECT_EQ(copy.sizeBytes, 5u);
        ASSERT_TRUE(copy.downloadedAt.has_value());
        EXPECT_EQ(*copy.downloadedAt, meta.at("createdAt").get<std::uint64_t>());
        ASSERT_TRUE(copy.endedAt.has_value());
        EXPECT_EQ(*copy.endedAt, meta.at("deadAt").get<std::uint64_t>());
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansLeavesOutLiveLockedAndForeignDirectories)
    {
        auto mgr = makeManager();
        auto* live = mgr->createInstance();
        ASSERT_NE(live, nullptr);
        writeFileOfSize(live->instanceDir() / "live.txt", 3u);
        auto otherRun = makeManager();
        auto* otherRunInstance = otherRun->createInstance();
        ASSERT_NE(otherRunInstance, nullptr);
        makeDeadBookkeepingInstanceDir(root_, "held-elsewhere", std::chrono::system_clock::now() - 1h);
        InstanceLock heldLock{bookkeepingPaths(root_ / "held-elsewhere").lock};
        fs::create_directories(root_ / "no-lock-dir");
        makeDeadBookkeepingInstanceDir(root_, "orphan-b", std::chrono::system_clock::now() - 1h);
        makeDeadBookkeepingInstanceDir(root_, "orphan-a", std::chrono::system_clock::now() - 2h);

        const auto copies = mgr->listOrphaned();

        ASSERT_EQ(copies.size(), 2u);
        EXPECT_EQ(copies[0].instanceId.value(), "orphan-a");
        EXPECT_EQ(copies[1].instanceId.value(), "orphan-b");
        EXPECT_EQ(mgr->countOrphaned(), copies.size());
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansSumsTheSizesBelowADirectory)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "tree-copy", std::chrono::system_clock::now() - 1h);
        const auto tree = root_ / "tree-copy" / "project";
        fs::create_directories(tree / "below");
        writeFileOfSize(tree / "first.bin", 100u);
        writeFileOfSize(tree / "below" / "second.bin", 23u);

        const auto copy = listedCopyOf(mgr->listOrphaned(), "tree-copy");

        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->name, "project");
        ASSERT_TRUE(copy->copyPath.has_value());
        EXPECT_EQ(copy->copyPath->native(), fs::weakly_canonical(tree));
        EXPECT_EQ(copy->sizeBytes, 123u);
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansNamesTheFirstOfSeveralEntries)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "several", std::chrono::system_clock::now() - 1h);
        writeFileOfSize(root_ / "several" / "zeta.txt", 1u);
        writeFileOfSize(root_ / "several" / "alpha.txt", 2u);

        const auto copy = listedCopyOf(mgr->listOrphaned(), "several");

        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->name, "alpha.txt");
        EXPECT_EQ(copy->sizeBytes, 3u);
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansIncludesAnInstanceWithoutRecordedOrigin)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "older-build", std::chrono::system_clock::now() - 1h);
        writeFileOfSize(root_ / "older-build" / "old.txt", 4u);

        const auto copy = listedCopyOf(mgr->listOrphaned(), "older-build");

        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->name, "old.txt");
        EXPECT_FALSE(copy->remotePath.has_value());
        EXPECT_EQ(copy->sizeBytes, 4u);
        EXPECT_TRUE(copy->downloadedAt.has_value());
        EXPECT_TRUE(copy->endedAt.has_value());
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansIncludesALegacyLayoutInstance)
    {
        auto mgr = makeManager();
        const auto deadAt = std::chrono::system_clock::now() - 1h;
        makeDeadInstanceDir(root_, "legacy-layout", deadAt);
        writeFileOfSize(root_ / "legacy-layout" / "report.txt", 7u);

        const auto copy = listedCopyOf(mgr->listOrphaned(), "legacy-layout");

        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->name, "report.txt");
        EXPECT_EQ(copy->sizeBytes, 7u);
        EXPECT_FALSE(copy->remotePath.has_value());
        ASSERT_TRUE(copy->endedAt.has_value());
        EXPECT_EQ(*copy->endedAt, static_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(deadAt)));
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansCountsContentNamedLikeLegacyBookkeeping)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "named-alike", std::chrono::system_clock::now() - 1h);
        writeFileOfSize(root_ / "named-alike" / "metadata.json", 6u);
        writeFileOfSize(root_ / "named-alike" / "named-alike.lock", 2u);

        const auto copy = listedCopyOf(mgr->listOrphaned(), "named-alike");

        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->name, "metadata.json");
        EXPECT_EQ(copy->sizeBytes, 8u);
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansIncludesAnEmptyInstance)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "empty-instance");

        const auto copy = listedCopyOf(mgr->listOrphaned(), "empty-instance");

        ASSERT_TRUE(copy.has_value());
        EXPECT_TRUE(copy->name.empty());
        EXPECT_EQ(copy->directory.native(), fs::weakly_canonical(root_ / "empty-instance"));
        EXPECT_FALSE(copy->copyPath.has_value());
        EXPECT_EQ(copy->sizeBytes, 0u);
        EXPECT_TRUE(copy->downloadedAt.has_value());
        EXPECT_FALSE(copy->endedAt.has_value());
        EXPECT_TRUE(readJson(bookkeepingPaths(root_ / "empty-instance").metadata)["deadAt"].is_null());
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansIgnoresAnInvalidRecordedOrigin)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "bad-origin", std::chrono::system_clock::now() - 1h);
        const auto metaPath = bookkeepingPaths(root_ / "bad-origin").metadata;
        auto meta = readJson(metaPath);
        meta["remotePath"] = "relative/path.txt";
        std::ofstream{metaPath} << meta.dump(4);

        const auto copy = listedCopyOf(mgr->listOrphaned(), "bad-origin");

        ASSERT_TRUE(copy.has_value());
        EXPECT_FALSE(copy->remotePath.has_value());
        EXPECT_TRUE(copy->endedAt.has_value());
    }

    TEST_F(TempDirInstanceManagerTest, ListingOrphansIncludesAnInstanceWithUnreadableMetadata)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "broken-metadata");
        std::ofstream{bookkeepingPaths(root_ / "broken-metadata").metadata} << "no json";
        writeFileOfSize(root_ / "broken-metadata" / "kept.txt", 9u);

        const auto copy = listedCopyOf(mgr->listOrphaned(), "broken-metadata");

        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->name, "kept.txt");
        EXPECT_EQ(copy->sizeBytes, 9u);
        EXPECT_FALSE(copy->remotePath.has_value());
        EXPECT_FALSE(copy->downloadedAt.has_value());
        EXPECT_FALSE(copy->endedAt.has_value());
    }

    TEST_F(TempDirInstanceManagerTest, RecordedOriginKeepsTheInstanceReadableForTheCleanup)
    {
        auto mgr = makeManager(0h);
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        ASSERT_TRUE(instance->recordOrigin(remotePathOf("/srv/notes.txt")).has_value());
        mgr->destroyInstance(instance->instanceId());
        ASSERT_EQ(mgr->countOrphaned(), 1u);

        mgr->manualCleanup();

        EXPECT_FALSE(fs::exists(fs::symlink_status(directory)));
    }

    // ---- removeOrphanedInstance behaviour ----

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanDeletesOnlyIt)
    {
        auto mgr = makeManager();
        auto* instance = mgr->createInstance();
        ASSERT_NE(instance, nullptr);
        const auto directory = instance->instanceDir();
        const auto id = instance->instanceId();
        fs::create_directories(directory / "tree");
        writeFileOfSize(directory / "tree" / "file.txt", 10u);
        mgr->destroyInstance(id);
        makeDeadBookkeepingInstanceDir(root_, "other-orphan", std::chrono::system_clock::now() - 1h);

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId(id));

        ASSERT_TRUE(removed.has_value()) << removed.error();
        EXPECT_FALSE(fs::exists(fs::symlink_status(directory)));
        EXPECT_TRUE(fs::exists(root_ / "other-orphan"));
        const auto copies = mgr->listOrphaned();
        ASSERT_EQ(copies.size(), 1u);
        EXPECT_EQ(copies.front().instanceId.value(), "other-orphan");
    }

    TEST_F(TempDirInstanceManagerTest, RemovingALegacyLayoutOrphanDeletesIt)
    {
        auto mgr = makeManager();
        makeDeadInstanceDir(root_, "legacy-orphan", std::chrono::system_clock::now() - 1h);
        writeFileOfSize(root_ / "legacy-orphan" / "doc.txt", 3u);

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId("legacy-orphan"));

        ASSERT_TRUE(removed.has_value()) << removed.error();
        EXPECT_FALSE(fs::exists(fs::symlink_status(root_ / "legacy-orphan")));
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesALiveInstance)
    {
        auto mgr = makeManager();
        auto* live = mgr->createInstance();
        ASSERT_NE(live, nullptr);
        writeFileOfSize(live->instanceDir() / "live.txt", 3u);

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId(live->instanceId()));

        ASSERT_FALSE(removed.has_value());
        EXPECT_NE(removed.error().find(live->instanceId()), std::string::npos) << removed.error();
        EXPECT_TRUE(fs::exists(live->instanceDir() / "live.txt"));
        EXPECT_EQ(mgr->findInstance(live->instanceId()), live);
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesAnInstanceOfAnotherRun)
    {
        auto mgr = makeManager();
        auto otherRun = makeManager();
        auto* foreign = otherRun->createInstance();
        ASSERT_NE(foreign, nullptr);
        writeFileOfSize(foreign->instanceDir() / "foreign.txt", 3u);

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId(foreign->instanceId()));

        ASSERT_FALSE(removed.has_value());
        EXPECT_TRUE(fs::exists(foreign->instanceDir() / "foreign.txt"));
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesALockedInstance)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "held-elsewhere", std::chrono::system_clock::now() - 1h);
        writeFileOfSize(root_ / "held-elsewhere" / "content.txt", 3u);
        InstanceLock heldLock{bookkeepingPaths(root_ / "held-elsewhere").lock};

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId("held-elsewhere"));

        ASSERT_FALSE(removed.has_value());
        EXPECT_NE(removed.error().find("held-elsewhere"), std::string::npos) << removed.error();
        EXPECT_TRUE(fs::exists(root_ / "held-elsewhere" / "content.txt"));
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesIdsThatAreNoPlainName)
    {
        const auto instancesRoot = root_ / "instances";
        fs::create_directories(instancesRoot);
        TempDirInstanceManager mgr{boost::asio::make_strand(ioc_.get_executor()), instancesRoot, 24h};
        makeDeadBookkeepingInstanceDir(root_, "outside", std::chrono::system_clock::now() - 1h);
        makeDeadBookkeepingInstanceDir(instancesRoot / "nested", "inner", std::chrono::system_clock::now() - 1h);
        const std::vector<std::string> ids{
            "../outside",
            "nested/inner",
            "nested\\inner",
            ".",
            "..",
            Paths::nativeToUtf8(root_ / "outside"),
        };

        for (auto const& id : ids)
        {
            const auto removed = mgr.removeOrphanedInstance(Ids::makeInstanceId(id));
            ASSERT_FALSE(removed.has_value()) << id;
            EXPECT_NE(removed.error().find("plain"), std::string::npos) << removed.error();
        }

        EXPECT_TRUE(fs::exists(bookkeepingPaths(root_ / "outside").lock));
        EXPECT_TRUE(fs::exists(bookkeepingPaths(instancesRoot / "nested" / "inner").lock));
        EXPECT_TRUE(fs::exists(instancesRoot));
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesAnEmptyId)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "orphan", std::chrono::system_clock::now() - 1h);

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId(""));

        ASSERT_FALSE(removed.has_value());
        EXPECT_NE(removed.error().find("empty"), std::string::npos) << removed.error();
        EXPECT_TRUE(fs::exists(root_ / "orphan"));
        EXPECT_TRUE(fs::exists(root_));
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesAMissingInstance)
    {
        auto mgr = makeManager();

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId("does-not-exist"));

        ASSERT_FALSE(removed.has_value());
        EXPECT_NE(removed.error().find("does-not-exist"), std::string::npos) << removed.error();
        EXPECT_NE(removed.error().find("does not exist"), std::string::npos) << removed.error();
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesADirectoryWithoutLock)
    {
        auto mgr = makeManager();
        fs::create_directories(root_ / "no-lock-dir");
        writeFileOfSize(root_ / "no-lock-dir" / "content.txt", 3u);

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId("no-lock-dir"));

        ASSERT_FALSE(removed.has_value());
        EXPECT_TRUE(fs::exists(root_ / "no-lock-dir" / "content.txt"));
    }

    TEST_F(TempDirInstanceManagerTest, RemovingAnOrphanRefusesALinkToAnOrphan)
    {
        auto mgr = makeManager();
        makeDeadBookkeepingInstanceDir(root_, "real-orphan", std::chrono::system_clock::now() - 1h);
        writeFileOfSize(root_ / "real-orphan" / "content.txt", 3u);
        std::error_code linkError;
        fs::create_directory_symlink(root_ / "real-orphan", root_ / "linked-orphan", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create symlinks here: " << linkError.message();

        const auto removed = mgr->removeOrphanedInstance(Ids::makeInstanceId("linked-orphan"));

        ASSERT_FALSE(removed.has_value());
        EXPECT_TRUE(fs::exists(fs::symlink_status(root_ / "linked-orphan")));
        EXPECT_TRUE(fs::exists(root_ / "real-orphan" / "content.txt"));
    }

} // namespace Test
