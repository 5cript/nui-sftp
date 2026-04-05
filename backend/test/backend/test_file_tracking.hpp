#pragma once

#include <backend/file_tracking/instance_lock.hpp>
#include <backend/file_tracking/instance_watch.hpp>
#include <backend/file_tracking/temp_dir_instancing.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>

#include <nlohmann/json.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace FileTracking;
using namespace std::chrono_literals;

extern std::filesystem::path programDirectory;

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
        auto root = programDirectory / "temp" / ("nui_sftp_ft_" + tag + "_" + suffix);
        fs::create_directories(root);
        return root;
    }

    /**
     * @brief Simulate the on-disk remnants of a dead TemporaryDirectoryInstance.
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

        nlohmann::json meta = {
            {"instanceId", instanceId},
            {"createdAt", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"deadAt", nullptr},
        };
        if (deadAt)
            meta["deadAt"] = std::chrono::system_clock::to_time_t(*deadAt);

        std::ofstream metaOut{dir / "metadata.json"};
        metaOut << meta.dump(4);
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
    // TemporaryDirectoryInstanceTest — requires a display
    // =========================================================================

    class TemporaryDirectoryInstanceTest : public ::testing::Test
    {
      protected:
        boost::asio::io_context ioc_;
        fs::path root_;

        void SetUp() override
        {
            if (!NuiEnv::instance().available())
                GTEST_SKIP() << "No display available; skipping TemporaryDirectoryInstance tests";
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
            auto& env = NuiEnv::instance();
            return std::make_unique<TemporaryDirectoryInstance>(
                TemporaryDirectoryInstance::Config{.tempRootDir = root_}, makeStrand(), *env.window, *env.hub
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

    TEST_F(TemporaryDirectoryInstanceTest, LockFileExistsInInstanceDir)
    {
        auto inst = makeInstance();
        auto lockPath = inst->instanceDir() / (inst->instanceId() + ".lock");
        EXPECT_TRUE(fs::exists(lockPath));
    }

    TEST_F(TemporaryDirectoryInstanceTest, LockFileIsHeldDuringLifetime)
    {
        auto inst = makeInstance();
        auto lockPath = inst->instanceDir() / (inst->instanceId() + ".lock");
        EXPECT_TRUE(InstanceLock::isLockedByAnother(lockPath));
    }

    TEST_F(TemporaryDirectoryInstanceTest, LockReleasedAfterDestruction)
    {
        fs::path lockPath;
        fs::path dir;
        {
            auto inst = makeInstance();
            dir = inst->instanceDir();
            lockPath = dir / (inst->instanceId() + ".lock");
            ASSERT_TRUE(InstanceLock::isLockedByAnother(lockPath));
        }
        EXPECT_FALSE(InstanceLock::isLockedByAnother(lockPath));
        fs::remove_all(dir);
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataFileCreatedOnConstruction)
    {
        auto inst = makeInstance();
        EXPECT_TRUE(fs::exists(inst->instanceDir() / "metadata.json"));
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataContainsCorrectInstanceId)
    {
        auto inst = makeInstance();
        std::ifstream fin{inst->instanceDir() / "metadata.json"};
        auto meta = nlohmann::json::parse(fin);
        EXPECT_EQ(meta.at("instanceId").get<std::string>(), inst->instanceId());
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataHasCreatedAt)
    {
        auto inst = makeInstance();
        std::ifstream fin{inst->instanceDir() / "metadata.json"};
        auto meta = nlohmann::json::parse(fin);
        ASSERT_TRUE(meta.contains("createdAt"));
        EXPECT_FALSE(meta["createdAt"].is_null());
    }

    TEST_F(TemporaryDirectoryInstanceTest, MetadataHasNullDeadAtInitially)
    {
        auto inst = makeInstance();
        std::ifstream fin{inst->instanceDir() / "metadata.json"};
        auto meta = nlohmann::json::parse(fin);
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
            metaPath = inst->instanceDir() / "metadata.json";
        }
        ASSERT_TRUE(fs::exists(metaPath));
        std::ifstream fin{metaPath};
        auto meta = nlohmann::json::parse(fin);
        ASSERT_TRUE(meta.contains("deadAt"));
        EXPECT_FALSE(meta["deadAt"].is_null());
        fs::remove_all(metaPath.parent_path());
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

    // =========================================================================
    // TempDirInstanceManagerTest — requires a display
    // =========================================================================

    class TempDirInstanceManagerTest : public ::testing::Test
    {
      protected:
        boost::asio::io_context ioc_;
        fs::path root_;

        void SetUp() override
        {
            if (!NuiEnv::instance().available())
                GTEST_SKIP() << "No display available; skipping TempDirInstanceManager tests";
            root_ = makeTestTempRoot("mgr");
        }

        void TearDown() override
        {
            if (!root_.empty())
                fs::remove_all(root_);
        }

        std::unique_ptr<TempDirInstanceManager> makeManager(std::chrono::hours retention = 24h)
        {
            auto& env = NuiEnv::instance();
            return std::make_unique<TempDirInstanceManager>(
                ioc_.get_executor(), *env.window, *env.hub, root_, retention
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

        auto metaPath = dir / "metadata.json";
        ASSERT_TRUE(fs::exists(metaPath));
        std::ifstream fin{metaPath};
        auto meta = nlohmann::json::parse(fin);
        EXPECT_FALSE(meta["deadAt"].is_null());
    }

    TEST_F(TempDirInstanceManagerTest, ManualCleanupWritesDeadAtWhenDeadAtIsNull)
    {
        auto mgr = makeManager(24h);
        std::string deadId = "dead-null-deadat";
        makeDeadInstanceDir(root_, deadId); // deadAt = null

        mgr->manualCleanup();

        std::ifstream fin{root_ / deadId / "metadata.json"};
        auto meta = nlohmann::json::parse(fin);
        EXPECT_FALSE(meta["deadAt"].is_null());
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

} // namespace Test
