#pragma once

#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/scan_node.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace SharedData::Sync::Test
{
    /**
     * @brief Test-only sink that captures emissions into flat vectors.  Mirrors the
     *        backend-side DiffTreeStore closely enough to make assertions easy.
     */
    struct CollectingSink : DiffSink
    {
        struct Record
        {
            DiffTreeNode node;
            std::string parentRelKey;
        };

        std::vector<Record> uploads;
        std::vector<Record> downloads;
        std::vector<Record> deletes;
        std::uint64_t lastProgress{0};

        void emitUpload(DiffTreeNode node, std::string const& parentRelKey) override
        {
            uploads.push_back(Record{.node = std::move(node), .parentRelKey = parentRelKey});
        }
        void emitDownload(DiffTreeNode node, std::string const& parentRelKey) override
        {
            downloads.push_back(Record{.node = std::move(node), .parentRelKey = parentRelKey});
        }
        void emitDelete(DiffTreeNode node, std::string const& parentRelKey) override
        {
            deletes.push_back(Record{.node = std::move(node), .parentRelKey = parentRelKey});
        }
        void onProgress(std::uint64_t entriesCompared) override
        {
            lastProgress = entriesCompared;
        }
    };

    inline bool hasRelKey(std::vector<CollectingSink::Record> const& records, std::string const& relKey)
    {
        return std::any_of(records.begin(), records.end(), [&](CollectingSink::Record const& record) {
            return record.node.relKey == relKey;
        });
    }

    inline CollectingSink::Record const*
    findRelKey(std::vector<CollectingSink::Record> const& records, std::string const& relKey)
    {
        auto iter = std::find_if(records.begin(), records.end(), [&](CollectingSink::Record const& record) {
            return record.node.relKey == relKey;
        });
        return iter == records.end() ? nullptr : &*iter;
    }

    /**
     * @brief Builds a leaf file node.
     */
    inline ScanNode makeFile(std::string const& name, std::uint64_t size, std::uint64_t mtime)
    {
        return ScanNode{
            .name = name,
            .type = FileType::Regular,
            .size = size,
            .mtime = mtime,
        };
    }

    /**
     * @brief Builds a symlink node with a given raw link target.
     */
    inline ScanNode makeSymlink(std::string const& name, std::filesystem::path const& target, std::uint64_t mtime = 0)
    {
        return ScanNode{
            .name = name,
            .type = FileType::Symlink,
            .size = 0,
            .mtime = mtime,
            .linkTarget = target,
        };
    }

    /**
     * @brief Builds a directory with the given children, sorts by name, and accumulates
     *        subtreeFileCount / subtreeByteTotal post-order (mirrors what
     *        @ref Utility::TreeDirectoryWalker does at scan time).
     */
    inline ScanNode makeDir(std::string const& name, std::vector<ScanNode> children, bool childrenKnown = true)
    {
        std::sort(children.begin(), children.end(), [](ScanNode const& lhs, ScanNode const& rhs) {
            return lhs.name < rhs.name;
        });

        ScanNode node{
            .name = name,
            .type = FileType::Directory,
            .children = std::move(children),
            .childrenKnown = childrenKnown,
        };

        for (auto const& child : node.children)
        {
            if (child.type == FileType::Directory)
            {
                node.subtreeFileCount += child.subtreeFileCount;
                node.subtreeByteTotal += child.subtreeByteTotal;
            }
            else
            {
                node.subtreeFileCount += 1;
                node.subtreeByteTotal += child.size;
            }
        }
        return node;
    }

    TEST(SyncDiffTests, EmptyTreesProduceNoEmits)
    {
        CollectingSink sink;
        diffScanTrees(makeDir("", {}), makeDir("", {}), {}, sink);
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
        EXPECT_TRUE(sink.deletes.empty());
    }

    TEST(SyncDiffTests, IdenticalTreesProduceNoEmits)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("file.txt", 17, 100)}),
            makeDir("", {makeFile("file.txt", 17, 100)}),
            {},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
        EXPECT_TRUE(sink.deletes.empty());
    }

    TEST(SyncDiffTests, LocalOnlyFileBecomesUploadInBothDirection)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("only_local.txt", 5, 100)}),
            makeDir("", {}),
            {},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.uploads.front().node.relKey, "only_local.txt");
        EXPECT_EQ(sink.uploads.front().node.action, Action::Upload);
        EXPECT_TRUE(sink.uploads.front().node.hasLocalSide);
        EXPECT_FALSE(sink.uploads.front().node.hasRemoteSide);
        EXPECT_EQ(sink.uploads.front().parentRelKey, "");
        EXPECT_TRUE(sink.downloads.empty());
        EXPECT_TRUE(sink.deletes.empty());
    }

    TEST(SyncDiffTests, RemoteOnlyFileBecomesDownloadInBothDirection)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {}),
            makeDir("", {makeFile("only_remote.txt", 5, 100)}),
            {},
            sink
        );
        ASSERT_EQ(sink.downloads.size(), 1u);
        EXPECT_EQ(sink.downloads.front().node.relKey, "only_remote.txt");
        EXPECT_EQ(sink.downloads.front().node.action, Action::Download);
    }

    TEST(SyncDiffTests, NewerLocalFileWinsInBothDirection)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("file.txt", 6, 200)}),
            makeDir("", {makeFile("file.txt", 5, 100)}),
            {},
            sink
        );
        EXPECT_EQ(sink.uploads.size(), 1u);
        EXPECT_TRUE(sink.downloads.empty());
    }

    TEST(SyncDiffTests, NewerRemoteFileWinsInBothDirection)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("file.txt", 6, 100)}),
            makeDir("", {makeFile("file.txt", 5, 200)}),
            {},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_EQ(sink.downloads.size(), 1u);
    }

    TEST(SyncDiffTests, UploadDirectionForcesUploadOnDifferingFile)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("file.txt", 6, 100)}),
            makeDir("", {makeFile("file.txt", 5, 200)}),
            {.direction = Direction::Upload},
            sink
        );
        EXPECT_EQ(sink.uploads.size(), 1u);
        EXPECT_TRUE(sink.downloads.empty());
    }

    TEST(SyncDiffTests, DownloadDirectionSuppressesUploads)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("only_local.txt", 5, 100)}),
            makeDir("", {}),
            {.direction = Direction::Download},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
        EXPECT_TRUE(sink.deletes.empty());
    }

    TEST(SyncDiffTests, DownloadDirectionWithDeleteRemovesLocalOnlyFile)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("only_local.txt", 5, 100)}),
            makeDir("", {}),
            {.direction = Direction::Download, .actionDelete = true},
            sink
        );
        ASSERT_EQ(sink.deletes.size(), 1u);
        EXPECT_EQ(sink.deletes.front().node.action, Action::DeleteLocal);
    }

    TEST(SyncDiffTests, UploadDirectionWithDeleteRemovesRemoteOnlyFile)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {}),
            makeDir("", {makeFile("only_remote.txt", 5, 100)}),
            {.direction = Direction::Upload, .actionDelete = true},
            sink
        );
        ASSERT_EQ(sink.deletes.size(), 1u);
        EXPECT_EQ(sink.deletes.front().node.action, Action::DeleteRemote);
    }

    TEST(SyncDiffTests, SameDirectoryOnBothSidesDoesNotEmitAnything)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir("subdir", {})}),
            makeDir("", {makeDir("subdir", {})}),
            {},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
        EXPECT_TRUE(sink.deletes.empty());
    }

    TEST(SyncDiffTests, OneSidedDirectoryEmitsSingleRowWithSubtreeCounts)
    {
        // Local has a directory tree with 3 files; remote is empty. A one-sided
        // directory should emit exactly one row with directChildCount/descendant counts
        // from the cached ScanNode metrics — no recursion during diff.
        auto subtree = makeDir("dir", {
            makeFile("a.txt", 10, 100),
            makeFile("b.txt", 20, 100),
            makeDir("inner", {makeFile("c.txt", 5, 100)}),
        });

        CollectingSink sink;
        diffScanTrees(
            makeDir("", {std::move(subtree)}),
            makeDir("", {}),
            {},
            sink
        );

        ASSERT_EQ(sink.uploads.size(), 1u);
        auto const& row = sink.uploads.front().node;
        EXPECT_EQ(row.relKey, "dir");
        EXPECT_TRUE(row.isDirectory);
        EXPECT_EQ(row.descendantItemCount, 3u);
        EXPECT_EQ(row.descendantByteTotal, 10u + 20u + 5u);
        EXPECT_EQ(row.directChildCount, 3u);
    }

    TEST(SyncDiffTests, RecursiveWalkEmitsEachDifferingChild)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {
                makeDir("sub", {
                    makeFile("a.txt", 10, 100),
                    makeFile("b.txt", 20, 100),
                }),
            }),
            makeDir("", {
                makeDir("sub", {}),
            }),
            {},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 2u);
        EXPECT_TRUE(hasRelKey(sink.uploads, "sub/a.txt"));
        EXPECT_TRUE(hasRelKey(sink.uploads, "sub/b.txt"));
        EXPECT_EQ(findRelKey(sink.uploads, "sub/a.txt")->parentRelKey, "sub");
    }

    TEST(SyncDiffTests, NonRecursiveSkipsDescendIntoSameNameDirectory)
    {
        // top.txt differs → upload. subdir exists on both sides but in non-recursive
        // mode the walk must NOT descend into it.
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {
                makeFile("top.txt", 5, 100),
                makeDir("subdir", {makeFile("nested.txt", 5, 100)}),
            }),
            makeDir("", {
                makeDir("subdir", {}),
            }),
            {.recursive = false},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.uploads.front().node.relKey, "top.txt");
    }

    TEST(SyncDiffTests, NonRecursiveDeleteHidesUnknownChildrenDirectory)
    {
        // Local has a directory that the scanner didn't descend into
        // (childrenKnown=false). With Direction::Download + actionDelete, we must
        // suppress the delete — deleting a subtree the user can't audit is unsafe.
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir("orphan_dir", {}, /*childrenKnown=*/false)}),
            makeDir("", {}),
            {.direction = Direction::Download, .actionDelete = true, .recursive = false},
            sink
        );
        EXPECT_TRUE(sink.deletes.empty());
    }

    TEST(SyncDiffTests, IgnoreHiddenDropsDotEntriesAtEveryLevel)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {
                makeFile("normal.txt", 5, 100),
                makeFile(".hidden", 5, 100),
                makeDir(".git", {makeFile("config", 5, 100)}),
            }),
            makeDir("", {}),
            {.ignoreHidden = true},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.uploads.front().node.relKey, "normal.txt");
    }

    TEST(SyncDiffTests, SymlinksWithSameTargetDoNotDiffer)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeSymlink("link", "/some/target", 100)}),
            makeDir("", {makeSymlink("link", "/some/target", 200)}),
            {},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
    }

    TEST(SyncDiffTests, SymlinksWithDifferentTargetsProduceDiff)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeSymlink("link", "/local/target", 200)}),
            makeDir("", {makeSymlink("link", "/remote/target", 100)}),
            {},
            sink
        );
        EXPECT_EQ(sink.uploads.size() + sink.downloads.size(), 1u);
    }

    TEST(SyncDiffTests, TypeMismatchCountsAsDifference)
    {
        auto localNode = makeFile("same_name", 5, 100);
        auto remoteNode = makeSymlink("same_name", "/elsewhere", 100);
        EXPECT_TRUE(entriesDiffer(localNode, remoteNode));
    }

    TEST(SyncDiffTests, ActionTogglesSuppressMatchingLists)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("only_local.txt", 5, 100)}),
            makeDir("", {makeFile("only_remote.txt", 5, 100)}),
            {.actionUpload = false, .actionDownload = false, .actionDelete = false},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
        EXPECT_TRUE(sink.deletes.empty());
    }

    TEST(SyncDiffTests, OneSidedFullVsEmptyIsCheaperThanEqualWalk)
    {
        // Sanity check on the algorithm's "prune one-sided subtrees" promise:
        // 1 local child with 100 deep descendants vs empty remote should visit
        // exactly one top-level compare, not 100.
        std::vector<ScanNode> deepChildren;
        deepChildren.reserve(100);
        for (int idx = 0; idx < 100; ++idx)
            deepChildren.push_back(makeFile("f" + std::to_string(idx), 1, 100));
        auto localTree = makeDir("", {makeDir("deep", std::move(deepChildren))});

        CollectingSink sink;
        diffScanTrees(localTree, makeDir("", {}), {}, sink);

        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.uploads.front().node.relKey, "deep");
        // Only the one top-level compare happened.
        EXPECT_EQ(sink.lastProgress, 1u);
    }
}
