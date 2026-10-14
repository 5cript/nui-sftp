#pragma once

#include <backend/sync/sync_baseline_store.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class SyncBaselineStoreTests : public ::testing::Test
    {
      protected:
        static SharedData::Sync::SyncRecord record(std::uint64_t size)
        {
            const SharedData::Sync::RecordedSide side{.type = SharedData::FileType::Regular, .size = size, .mtime = 7};
            return {.local = side, .remote = side};
        }

        Utility::TemporaryDirectory directory_{temporaryRoot, true};
        SyncBaselineStore store_{directory_.path() / "baselines"};
        std::string pairKey_{SyncBaselineStore::pairKey(
            "saved",
            Paths::LocalPath::fromUtf8("/local/root", Paths::LocalPlatform::Posix).value(),
            Paths::RemotePath::make("/remote/root").value()
        )};
    };

    TEST_F(SyncBaselineStoreTests, APairWithoutFileHasNoBaseline)
    {
        EXPECT_FALSE(store_.load(pairKey_));
    }

    TEST_F(SyncBaselineStoreTests, UpdatesAddToWhatIsStored)
    {
        ASSERT_TRUE(store_.update(
            pairKey_,
            [](SharedData::Sync::SyncBaseline& baseline)
            {
                baseline.records.insert_or_assign("a.txt", record(1));
            }
        ));
        ASSERT_TRUE(
            SyncBaselineStore{directory_.path() / "baselines"}.update(
                pairKey_,
                [](SharedData::Sync::SyncBaseline& baseline)
                {
                    EXPECT_TRUE(baseline.records.contains("a.txt"));
                    baseline.records.insert_or_assign("b.txt", record(2));
                }
            )
        );
        const auto loaded = store_.load(pairKey_);
        ASSERT_TRUE(loaded);
        EXPECT_EQ(loaded->records.size(), 2u);
        EXPECT_EQ(loaded->records.at("b.txt"), record(2));
    }

    TEST_F(SyncBaselineStoreTests, AnEmptyUpdateStillGivesThePairAHistory)
    {
        ASSERT_TRUE(store_.update(pairKey_, [](SharedData::Sync::SyncBaseline&) {}));
        const auto loaded = store_.load(pairKey_);
        ASSERT_TRUE(loaded);
        EXPECT_TRUE(loaded->records.empty());
    }

    TEST_F(SyncBaselineStoreTests, ADamagedFileIsAHistoryWithoutRecords)
    {
        std::filesystem::create_directories(directory_.path() / "baselines");
        std::ofstream{SyncBaselineStore::fileOf(directory_.path() / "baselines", pairKey_)} << "{not json";
        const auto loaded = store_.load(pairKey_);
        ASSERT_TRUE(loaded) << "a pair whose records are lost protects every delete";
        EXPECT_TRUE(loaded->records.empty());
    }

    TEST_F(SyncBaselineStoreTests, AFileOfAnotherPairIsNeitherReadNorReplaced)
    {
        const auto otherKey = std::string{"other"};
        ASSERT_TRUE(store_.update(
            otherKey,
            [](SharedData::Sync::SyncBaseline& baseline)
            {
                baseline.records.insert_or_assign("other.txt", record(3));
            }
        ));
        // Collide on purpose by moving the other pair's file to this pair's name.
        std::filesystem::rename(
            SyncBaselineStore::fileOf(directory_.path() / "baselines", otherKey),
            SyncBaselineStore::fileOf(directory_.path() / "baselines", pairKey_)
        );
        EXPECT_FALSE(store_.load(pairKey_));
        EXPECT_FALSE(store_.update(pairKey_, [](SharedData::Sync::SyncBaseline&) {}));
    }

    TEST_F(SyncBaselineStoreTests, PairKeysNameTheSessionAndBothRoots)
    {
        EXPECT_EQ(pairKey_, "saved\n/local/root\n/remote/root");
        EXPECT_NE(
            SyncBaselineStore::fileOf(directory_.path(), pairKey_),
            SyncBaselineStore::fileOf(directory_.path(), "saved\n/local/root\n/remote/other")
        );
    }

    TEST_F(SyncBaselineStoreTests, PruningRemovesThePairsOfSessionsThatAreGone)
    {
        const auto local = Paths::LocalPath::fromUtf8("/local", Paths::LocalPlatform::Posix).value();
        const auto remote = Paths::RemotePath::make("/remote").value();
        const auto kept = SyncBaselineStore::pairKey("kept", local, remote);
        const auto gone = SyncBaselineStore::pairKey("gone", local, remote);
        ASSERT_TRUE(store_.update(kept, [](SharedData::Sync::SyncBaseline&) {}));
        ASSERT_TRUE(store_.update(gone, [](SharedData::Sync::SyncBaseline&) {}));
        std::ofstream{directory_.path() / "baselines" / "unrelated.txt"} << "x";

        EXPECT_EQ(store_.pruneAllBut({"kept", "other"}), 1u);
        EXPECT_TRUE(store_.load(kept));
        EXPECT_FALSE(store_.load(gone));
        EXPECT_TRUE(std::filesystem::exists(directory_.path() / "baselines" / "unrelated.txt"));
    }

    TEST_F(SyncBaselineStoreTests, PruningWithoutADirectoryRemovesNothing)
    {
        EXPECT_EQ(SyncBaselineStore{directory_.path() / "none"}.pruneAllBut({}), 0u);
    }
}
