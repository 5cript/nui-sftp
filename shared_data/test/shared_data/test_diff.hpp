#pragma once

#include <shared_data/sync/diff.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace SharedData::Sync::Test
{
    inline DirectoryEntry
    makeEntry(std::string const& relPath, FileType type, std::uint64_t size = 0, std::uint64_t mtime = 0,
              std::filesystem::path const& root = {})
    {
        DirectoryEntry entry{};
        entry.path = relPath;
        if (!root.empty())
            entry.fullPath = root / relPath;
        entry.type = type;
        entry.size = size;
        entry.mtime = mtime;
        return entry;
    }

    inline std::vector<DirectoryEntry>
    makeScan(std::filesystem::path const& root, std::vector<DirectoryEntry> children)
    {
        std::vector<DirectoryEntry> entries;
        entries.reserve(children.size() + 1);
        // Index 0 is the scan root (excluded from buildEntryMap).
        DirectoryEntry rootEntry{};
        rootEntry.path = root;
        rootEntry.type = FileType::Directory;
        entries.push_back(std::move(rootEntry));
        for (auto& child : children)
        {
            if (child.fullPath.empty())
                child.fullPath = root / child.path;
            entries.push_back(std::move(child));
        }
        return entries;
    }

    inline bool hasRelKey(std::vector<DiffEntry> const& entries, std::string const& relKey)
    {
        return std::any_of(entries.begin(), entries.end(),
            [&](DiffEntry const& entry) { return entry.relKey == relKey; });
    }

    inline DiffEntry const* findRelKey(std::vector<DiffEntry> const& entries, std::string const& relKey)
    {
        auto iter = std::find_if(entries.begin(), entries.end(),
            [&](DiffEntry const& entry) { return entry.relKey == relKey; });
        return iter == entries.end() ? nullptr : &*iter;
    }

    TEST(SyncDiffTests, EmptyInputsProduceNoDiff)
    {
        const auto result = computeSyncDiff("/local", "/remote", {}, {}, {});
        EXPECT_TRUE(result.uploads.empty());
        EXPECT_TRUE(result.downloads.empty());
        EXPECT_TRUE(result.deletes.empty());
    }

    TEST(SyncDiffTests, IdenticalTreesProduceNoDiff)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("file.txt", FileType::Regular, 17, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {makeEntry("file.txt", FileType::Regular, 17, 100, remoteRoot)});

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        EXPECT_TRUE(result.uploads.empty());
        EXPECT_TRUE(result.downloads.empty());
        EXPECT_TRUE(result.deletes.empty());
    }

    TEST(SyncDiffTests, LocalOnlyEntryBecomesUploadInBothDirection)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("only_local.txt", FileType::Regular, 5, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {});

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        ASSERT_EQ(result.uploads.size(), 1u);
        EXPECT_EQ(result.uploads.front().relKey, "only_local.txt");
        EXPECT_EQ(result.uploads.front().action, Action::Upload);
        EXPECT_TRUE(result.uploads.front().local.has_value());
        EXPECT_FALSE(result.uploads.front().remote.has_value());
        EXPECT_TRUE(result.downloads.empty());
        EXPECT_TRUE(result.deletes.empty());
    }

    TEST(SyncDiffTests, RemoteOnlyEntryBecomesDownloadInBothDirection)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {});
        auto remote = makeScan(remoteRoot, {makeEntry("only_remote.txt", FileType::Regular, 5, 100, remoteRoot)});

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        ASSERT_EQ(result.downloads.size(), 1u);
        EXPECT_EQ(result.downloads.front().relKey, "only_remote.txt");
        EXPECT_EQ(result.downloads.front().action, Action::Download);
    }

    TEST(SyncDiffTests, NewerLocalEntryWinsInBothDirection)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("file.txt", FileType::Regular, 5, 200, localRoot)});
        auto remote = makeScan(remoteRoot, {makeEntry("file.txt", FileType::Regular, 5, 100, remoteRoot)});
        // Force a difference in size so the entries are considered to differ; the
        // direction is then chosen by mtime.
        local[1].size = 6;

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        EXPECT_EQ(result.uploads.size(), 1u);
        EXPECT_TRUE(result.downloads.empty());
    }

    TEST(SyncDiffTests, NewerRemoteEntryWinsInBothDirection)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("file.txt", FileType::Regular, 6, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {makeEntry("file.txt", FileType::Regular, 5, 200, remoteRoot)});

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        EXPECT_TRUE(result.uploads.empty());
        EXPECT_EQ(result.downloads.size(), 1u);
    }

    TEST(SyncDiffTests, UploadDirectionForcesUploadOnDifferingEntry)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        // Remote has a newer mtime, but Direction::Upload must override.
        auto local = makeScan(localRoot, {makeEntry("file.txt", FileType::Regular, 6, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {makeEntry("file.txt", FileType::Regular, 5, 200, remoteRoot)});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote, {.direction = Direction::Upload}
        );
        EXPECT_EQ(result.uploads.size(), 1u);
        EXPECT_TRUE(result.downloads.empty());
    }

    TEST(SyncDiffTests, DownloadDirectionSuppressesUploads)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("only_local.txt", FileType::Regular, 5, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote, {.direction = Direction::Download}
        );
        EXPECT_TRUE(result.uploads.empty());
        EXPECT_TRUE(result.downloads.empty());
        EXPECT_TRUE(result.deletes.empty());
    }

    TEST(SyncDiffTests, DownloadDirectionWithDeleteRemovesLocalOnlyEntry)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("only_local.txt", FileType::Regular, 5, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote,
            {.direction = Direction::Download, .actionDelete = true}
        );
        ASSERT_EQ(result.deletes.size(), 1u);
        EXPECT_EQ(result.deletes.front().action, Action::DeleteLocal);
    }

    TEST(SyncDiffTests, UploadDirectionWithDeleteRemovesRemoteOnlyEntry)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {});
        auto remote = makeScan(remoteRoot, {makeEntry("only_remote.txt", FileType::Regular, 5, 100, remoteRoot)});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote,
            {.direction = Direction::Upload, .actionDelete = true}
        );
        ASSERT_EQ(result.deletes.size(), 1u);
        EXPECT_EQ(result.deletes.front().action, Action::DeleteRemote);
    }

    TEST(SyncDiffTests, DirectoriesPresentOnBothSidesDoNotProduceDiff)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("subdir", FileType::Directory, 0, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {makeEntry("subdir", FileType::Directory, 0, 200, remoteRoot)});

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        EXPECT_TRUE(result.uploads.empty());
        EXPECT_TRUE(result.downloads.empty());
    }

    TEST(SyncDiffTests, NonRecursiveDropsNestedEntries)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {
            makeEntry("top.txt", FileType::Regular, 5, 100, localRoot),
            makeEntry("subdir/nested.txt", FileType::Regular, 5, 100, localRoot),
        });
        auto remote = makeScan(remoteRoot, {});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote, {.recursive = false}
        );
        ASSERT_EQ(result.uploads.size(), 1u);
        EXPECT_EQ(result.uploads.front().relKey, "top.txt");
    }

    TEST(SyncDiffTests, NonRecursiveDeleteHidesDirectoryEntries)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        // Local-only directory + Direction::Download + actionDelete; without recursive
        // mode the directory is hidden to avoid a recursive delete the user can't audit.
        auto local = makeScan(localRoot, {makeEntry("orphan_dir", FileType::Directory, 0, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote,
            {.direction = Direction::Download, .actionDelete = true, .recursive = false}
        );
        EXPECT_TRUE(result.deletes.empty());
    }

    TEST(SyncDiffTests, IgnoreHiddenDropsDotEntriesAndTrees)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {
            makeEntry("normal.txt", FileType::Regular, 5, 100, localRoot),
            makeEntry(".hidden", FileType::Regular, 5, 100, localRoot),
            makeEntry(".git/config", FileType::Regular, 5, 100, localRoot),
        });
        auto remote = makeScan(remoteRoot, {});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote, {.ignoreHidden = true}
        );
        ASSERT_EQ(result.uploads.size(), 1u);
        EXPECT_EQ(result.uploads.front().relKey, "normal.txt");
    }

    TEST(SyncDiffTests, SymlinksWithSameTargetDoNotDiffer)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto localLink = makeEntry("link", FileType::Symlink, 0, 100, localRoot);
        localLink.linkTarget = std::filesystem::path{"/some/target"};
        auto remoteLink = makeEntry("link", FileType::Symlink, 99, 200, remoteRoot);
        remoteLink.linkTarget = std::filesystem::path{"/some/target"};

        auto local = makeScan(localRoot, {localLink});
        auto remote = makeScan(remoteRoot, {remoteLink});

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        EXPECT_TRUE(result.uploads.empty());
        EXPECT_TRUE(result.downloads.empty());
    }

    TEST(SyncDiffTests, SymlinksWithDifferentTargetsProduceDiff)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto localLink = makeEntry("link", FileType::Symlink, 0, 200, localRoot);
        localLink.linkTarget = std::filesystem::path{"/local/target"};
        auto remoteLink = makeEntry("link", FileType::Symlink, 0, 100, remoteRoot);
        remoteLink.linkTarget = std::filesystem::path{"/remote/target"};

        auto local = makeScan(localRoot, {localLink});
        auto remote = makeScan(remoteRoot, {remoteLink});

        const auto result = computeSyncDiff(localRoot, remoteRoot, local, remote, {});
        EXPECT_EQ(result.uploads.size() + result.downloads.size(), 1u);
    }

    TEST(SyncDiffTests, TypeMismatchAlwaysCountsAsDifference)
    {
        DirectoryEntry localEntry{};
        localEntry.type = FileType::Regular;
        localEntry.size = 5;
        localEntry.mtime = 100;

        DirectoryEntry remoteEntry{};
        remoteEntry.type = FileType::Symlink;
        remoteEntry.size = 5;
        remoteEntry.mtime = 100;

        EXPECT_TRUE(entriesDiffer(localEntry, remoteEntry));
    }

    TEST(SyncDiffTests, ActionTogglesSuppressMatchingLists)
    {
        const std::filesystem::path localRoot = "/local";
        const std::filesystem::path remoteRoot = "/remote";

        auto local = makeScan(localRoot, {makeEntry("only_local.txt", FileType::Regular, 5, 100, localRoot)});
        auto remote = makeScan(remoteRoot, {makeEntry("only_remote.txt", FileType::Regular, 5, 100, remoteRoot)});

        const auto result = computeSyncDiff(
            localRoot, remoteRoot, local, remote,
            {.actionUpload = false, .actionDownload = false, .actionDelete = false}
        );
        EXPECT_TRUE(result.uploads.empty());
        EXPECT_TRUE(result.downloads.empty());
        EXPECT_TRUE(result.deletes.empty());
    }
}
