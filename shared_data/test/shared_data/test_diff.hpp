#pragma once

#include <shared_data/sync/case_fold.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/scan_node.hpp>

#include <fmt/format.h>
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
        std::vector<BlockedEntry> blocked;
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
        void onBlocked(std::string const& relKey, BlockReason reason) override
        {
            blocked.push_back(BlockedEntry{.relKey = relKey, .reason = reason});
        }
        void onProgress(std::uint64_t entriesCompared) override
        {
            lastProgress = entriesCompared;
        }
    };

    /**
     * @brief Builds a directory whose listing failed: its content is unknown.
     */
    inline ScanNode makeUnreadableDir(std::string const& name)
    {
        return ScanNode{
            .name = name,
            .type = FileType::Directory,
            .childrenKnown = false,
            .availability = Availability::Unreadable,
        };
    }

    /**
     * @brief Builds an entry the scan keeps but never transfers, like a FIFO.
     */
    inline ScanNode makeExcluded(std::string const& name, FileType type = FileType::Fifo)
    {
        return ScanNode{
            .name = name,
            .type = type,
            .availability = Availability::Excluded,
        };
    }

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
    inline ScanNode makeSymlink(std::string const& name, std::string const& target, std::uint64_t mtime = 0)
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

    TEST(SyncDiffTests, DownloadDirectionForcesDownloadOnDifferingFile)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("file.txt", 6, 200)}),
            makeDir("", {makeFile("file.txt", 5, 100)}),
            {.direction = Direction::Download},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        ASSERT_EQ(sink.downloads.size(), 1u);
        EXPECT_EQ(sink.downloads.front().node.action, Action::Download);
    }

    TEST(SyncDiffTests, EqualMtimeWithDifferentSizeFavorsUpload)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("file.txt", 6, 100)}), makeDir("", {makeFile("file.txt", 5, 100)}), {}, sink
        );
        EXPECT_EQ(sink.uploads.size(), 1u);
        EXPECT_TRUE(sink.downloads.empty());
    }

    TEST(SyncDiffTests, SameSizeAndMtimeCountsAsEqualEvenIfContentDiffers)
    {
        // The diff is stat based; documents that content changes keeping size and mtime stay invisible.
        EXPECT_FALSE(entriesDiffer(makeFile("same_stat.c", 4, 100), makeFile("same_stat.c", 4, 100)));
    }

    TEST(SyncDiffTests, TwoSidedRowCarriesBothSidesAndTransfersTheSourceSize)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("file.txt", 10, 100)}), makeDir("", {makeFile("file.txt", 25, 200)}), {}, sink
        );
        ASSERT_EQ(sink.downloads.size(), 1u);
        auto const& row = sink.downloads.front().node;
        EXPECT_TRUE(row.hasLocalSide);
        EXPECT_TRUE(row.hasRemoteSide);
        EXPECT_EQ(row.localSize, 10u);
        EXPECT_EQ(row.remoteSize, 25u);
        EXPECT_EQ(row.localMtime, 100u);
        EXPECT_EQ(row.remoteMtime, 200u);
        EXPECT_EQ(row.descendantByteTotal, 25u);
        EXPECT_FALSE(row.isDirectory);
        EXPECT_FALSE(row.typeConflict);
    }

    TEST(SyncDiffTests, LocalDirectoryAgainstOlderRemoteFileIsADirectoryUpload)
    {
        auto localDirectory = makeDir("conflict", {makeFile("inside.txt", 7, 300), makeFile("more.txt", 3, 300)});
        localDirectory.mtime = 300;
        CollectingSink sink;
        diffScanTrees(makeDir("", {std::move(localDirectory)}), makeDir("", {makeFile("conflict", 14, 100)}), {}, sink);
        ASSERT_EQ(sink.uploads.size(), 1u);
        auto const& row = sink.uploads.front().node;
        EXPECT_TRUE(row.typeConflict);
        EXPECT_TRUE(row.isDirectory) << "the transferred side is a directory";
        EXPECT_EQ(row.directChildCount, 2u);
        EXPECT_EQ(row.descendantItemCount, 2u);
        EXPECT_EQ(row.descendantByteTotal, 10u);
        EXPECT_TRUE(sink.downloads.empty());
    }

    TEST(SyncDiffTests, NewerRemoteFileAgainstLocalDirectoryIsAFileDownload)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir("conflict", {makeFile("inside.txt", 7, 100)})}),
            makeDir("", {makeFile("conflict", 14, 300)}),
            {},
            sink
        );
        ASSERT_EQ(sink.downloads.size(), 1u);
        auto const& row = sink.downloads.front().node;
        EXPECT_TRUE(row.typeConflict);
        EXPECT_FALSE(row.isDirectory);
        EXPECT_EQ(row.descendantByteTotal, 14u);
    }

    TEST(SyncDiffTests, DeepDifferencesEmitOnlyTheLeavesWithTheirParentKeys)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir("a", {makeDir("b", {makeDir("c", {makeFile("deep.txt", 1, 200)})})})}),
            makeDir("", {makeDir("a", {makeDir("b", {makeDir("c", {makeFile("deep.txt", 2, 100)})})})}),
            {},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.uploads.front().node.relKey, "a/b/c/deep.txt");
        EXPECT_EQ(sink.uploads.front().node.name, "deep.txt");
        EXPECT_EQ(sink.uploads.front().parentRelKey, "a/b/c");
    }

    TEST(SyncDiffTests, NamesSharingAPrefixPairUpByByteOrder)
    {
        // '-' (0x2D) and '.' (0x2E) sort before '/', so "a", "a-b" and "a.b" interleave with "a/...".
        CollectingSink sink;
        diffScanTrees(
            makeDir(
                "",
                {
                    makeDir("a", {makeFile("file.txt", 1, 100)}),
                    makeDir("a-b", {makeFile("file.txt", 1, 100)}),
                    makeFile("a.b", 1, 200),
                }
            ),
            makeDir(
                "",
                {
                    makeDir("a", {makeFile("file.txt", 1, 100)}),
                    makeFile("a.b", 1, 100),
                    makeFile("b", 1, 100),
                }
            ),
            {},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 2u);
        EXPECT_TRUE(hasRelKey(sink.uploads, "a-b"));
        EXPECT_TRUE(hasRelKey(sink.uploads, "a.b"));
        ASSERT_EQ(sink.downloads.size(), 1u);
        EXPECT_EQ(sink.downloads.front().node.relKey, "b");
    }

    TEST(SyncDiffTests, UnicodeNamesPairUp)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir(
                "",
                {makeDir(
                    "\xc3\xbc"
                    "bung",
                    {makeFile("\xe3\x83\x8a.txt", 1, 200)}
                )}
            ),
            makeDir(
                "",
                {makeDir(
                    "\xc3\xbc"
                    "bung",
                    {makeFile("\xe3\x83\x8a.txt", 1, 100)}
                )}
            ),
            {},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(
            sink.uploads.front().node.relKey,
            "\xc3\xbc"
            "bung/\xe3\x83\x8a.txt"
        );
    }

    TEST(SyncDiffTests, IgnoreHiddenSkipsHiddenDirectoriesOnBothSides)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir(".git", {makeFile("HEAD", 1, 200)}), makeDir("src", {makeFile(".env", 1, 200)})}),
            makeDir("", {makeDir(".git", {makeFile("HEAD", 2, 100)}), makeDir("src", {})}),
            {.ignoreHidden = true},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
    }

    TEST(SyncDiffTests, HiddenEntriesShowWithoutIgnoreHidden)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir("src", {makeFile(".env", 1, 200)})}),
            makeDir("", {makeDir("src", {}), makeFile(".remote_only", 1, 100)}),
            {},
            sink
        );
        EXPECT_TRUE(hasRelKey(sink.uploads, "src/.env"));
        EXPECT_TRUE(hasRelKey(sink.downloads, ".remote_only"));
    }

    TEST(SyncDiffTests, SymlinksWithoutKnownTargetsDoNotDiffer)
    {
        auto localLink = makeSymlink("link", "/target", 100);
        auto remoteLink = makeSymlink("link", "/other", 200);
        remoteLink.linkTarget.reset();
        EXPECT_FALSE(entriesDiffer(localLink, remoteLink));
    }

    TEST(SyncDiffTests, SymlinkTargetsDifferingOnlyInRepeatedSeparatorsDoNotDiffer)
    {
        EXPECT_FALSE(entriesDiffer(makeSymlink("link", "../a//b"), makeSymlink("link", "../a/b")));
        EXPECT_TRUE(entriesDiffer(makeSymlink("link", "a/b"), makeSymlink("link", "a/b/c")));
    }

    TEST(SyncDiffTests, DirectoryAgainstSymlinkIsATypeConflict)
    {
        EXPECT_TRUE(entriesDiffer(makeDir("name", {}), makeSymlink("name", "/elsewhere")));
    }

    TEST(SyncDiffTests, DeleteKeepsKnownDirectoriesAndSkipsUnknownOnes)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {}),
            makeDir(
                "",
                {
                    makeDir("known", {makeFile("x", 1, 100)}),
                    makeDir("unknown", {}, /*childrenKnown=*/false),
                    makeFile("file", 1, 100),
                }
            ),
            {.direction = Direction::Upload, .actionDelete = true},
            sink
        );
        ASSERT_EQ(sink.deletes.size(), 2u);
        EXPECT_TRUE(hasRelKey(sink.deletes, "known"));
        EXPECT_TRUE(hasRelKey(sink.deletes, "file"));
        EXPECT_FALSE(hasRelKey(sink.deletes, "unknown"));
        EXPECT_EQ(findRelKey(sink.deletes, "known")->node.action, Action::DeleteRemote);
    }

    TEST(SyncDiffTests, BothDirectionNeverDeletes)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("local", 1, 100)}),
            makeDir("", {makeFile("remote", 1, 100)}),
            {.actionDelete = true},
            sink
        );
        EXPECT_TRUE(sink.deletes.empty());
        EXPECT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.downloads.size(), 1u);
    }

    TEST(SyncDiffTests, UploadToggleOffKeepsDownloads)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("local", 1, 100), makeFile("shared", 1, 300)}),
            makeDir("", {makeFile("remote", 1, 100), makeFile("shared", 2, 200)}),
            {.actionUpload = false},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty()) << "newer local file must not turn into a download";
        ASSERT_EQ(sink.downloads.size(), 1u);
        EXPECT_EQ(sink.downloads.front().node.relKey, "remote");
    }

    TEST(SyncDiffTests, ProgressFiresAtCheckpointsAndOnceAtTheEnd)
    {
        struct ProgressSink : CollectingSink
        {
            std::vector<std::uint64_t> reports;
            void onProgress(std::uint64_t entriesCompared) override
            {
                reports.push_back(entriesCompared);
            }
        };

        std::vector<ScanNode> files;
        for (int index = 0; index < 1100; ++index)
            files.push_back(makeFile(fmt::format("f{:04}", index), 1, 100));
        auto tree = makeDir("", std::move(files));

        ProgressSink sink;
        diffScanTrees(tree, tree, {}, sink);
        EXPECT_EQ(sink.reports, (std::vector<std::uint64_t>{512, 1024, 1100}));
    }

    TEST(SyncDiffTests, CancelStopsTheWalkEarly)
    {
        struct CancellingSink : CollectingSink
        {
            bool cancelled() const override
            {
                return uploads.size() >= 3;
            }
        };

        std::vector<ScanNode> files;
        for (int index = 0; index < 100; ++index)
            files.push_back(makeFile(fmt::format("f{:03}", index), 1, 100));

        CancellingSink sink;
        diffScanTrees(makeDir("", std::move(files)), makeDir("", {}), {}, sink);
        EXPECT_EQ(sink.uploads.size(), 3u);
    }

    TEST(SyncDiffTests, NonRecursiveDiffNeverDeletesDirectoriesEvenWhenScanned)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {}),
            makeDir("", {makeDir("scanned", {makeFile("x", 1, 100)}), makeFile("file", 1, 100)}),
            {.direction = Direction::Upload, .actionDelete = true, .recursive = false},
            sink
        );
        ASSERT_EQ(sink.deletes.size(), 1u);
        EXPECT_EQ(sink.deletes.front().node.relKey, "file");
    }

    TEST(SyncDiffTests, NonRecursiveDiffEmitsOneSidedDirectoriesWithoutContent)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir("new", {makeFile("a", 5, 100), makeFile("b", 5, 100)})}),
            makeDir("", {}),
            {.recursive = false},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        auto const& row = sink.uploads.front().node;
        EXPECT_TRUE(row.isDirectory);
        EXPECT_EQ(row.directChildCount, 0u);
        EXPECT_EQ(row.descendantItemCount, 0u);
        EXPECT_EQ(row.descendantByteTotal, 0u);
    }

    TEST(SyncDiffTests, NamesDifferingOnlyInCaseAcrossSidesAreBlocked)
    {
        // On a case insensitive server "Foo" replaces "foo", the delete of "foo" would then remove the upload.
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeFile("Foo", 3, 200), makeFile("other", 1, 100)}),
            makeDir("", {makeFile("foo", 3, 100)}),
            {.direction = Direction::Upload, .actionDelete = true},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.uploads.front().node.relKey, "other");
        EXPECT_TRUE(sink.deletes.empty());
        ASSERT_EQ(sink.blocked.size(), 2u);
        for (auto const& blocked : sink.blocked)
            EXPECT_EQ(blocked.reason, BlockReason::CaseConflict) << blocked.relKey;
    }

    TEST(SyncCaseFoldTests, LettersOfEveryCoveredScriptFoldToOneKey)
    {
        EXPECT_EQ(caseFoldedName("README.md"), "readme.md");
        EXPECT_EQ(caseFoldedName("\xC3\x84PFEL"), caseFoldedName("\xC3\xA4pfel")) << "Ä and ä";
        EXPECT_EQ(caseFoldedName("ŁÓDŹ"), caseFoldedName("łódź")) << "Latin Extended-A";
        EXPECT_EQ(
            caseFoldedName("\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x9F\xCE\xA3"),
            caseFoldedName("\xCF\x83\xCE\xBF\xCF\x86\xCE\xBF\xCF\x82")
        ) << "ΣΟΦΟΣ and σοφος with a final sigma";
        EXPECT_EQ(caseFoldedName("\xD0\x81\xD0\x96"), caseFoldedName("\xD1\x91\xD0\xB6")) << "ЁЖ and ёж";
        EXPECT_NE(caseFoldedName("a"), caseFoldedName("b"));
        EXPECT_EQ(caseFoldedName("caf\xFF"), "caf\xFF") << "text that is not UTF-8 is its own key";
    }

    TEST(SyncCaseFoldTests, ComposedAndDecomposedSpellingsFoldToOneKey)
    {
        EXPECT_EQ(caseFoldedName("caf\xC3\xA9"), caseFoldedName("cafe\xCC\x81")) << "é as NFC and NFD";
        EXPECT_EQ(caseFoldedName("CAF\xC3\x89"), caseFoldedName("cafe\xCC\x81")) << "É folds to e and the accent";
        EXPECT_EQ(caseFoldedName("\xCE\x90"), caseFoldedName("\xCE\xB9\xCC\x88\xCC\x81")) << "ΐ decomposes twice";
        EXPECT_FALSE(foldingChanges("plain-name_1.txt"));
        EXPECT_TRUE(foldingChanges("Name"));
        EXPECT_TRUE(foldingChanges("caf\xC3\xA9"));
    }

    TEST(SyncDiffTests, NonAsciiNamesDifferingOnlyInCaseOrNormalizationAreBlocked)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir(
                "", {makeFile("\xC3\x84pfel.txt", 1, 100), makeFile("cafe\xCC\x81", 1, 100), makeFile("other", 1, 100)}
            ),
            makeDir("", {makeFile("\xC3\xA4pfel.txt", 2, 100), makeFile("caf\xC3\xA9", 2, 100)}),
            {.direction = Direction::Upload, .actionDelete = true, .remoteCaseInsensitive = true},
            sink
        );
        ASSERT_EQ(sink.uploads.size(), 1u);
        EXPECT_EQ(sink.uploads.front().node.relKey, "other");
        EXPECT_TRUE(sink.deletes.empty());
        EXPECT_EQ(sink.blocked.size(), 4u);
        for (auto const& blocked : sink.blocked)
            EXPECT_EQ(blocked.reason, BlockReason::CaseConflict) << blocked.relKey;
    }

    TEST(SyncDiffTests, RemoteCaseVariantsAreBlockedOnlyForACaseFoldingLocalFilesystem)
    {
        const auto local = makeDir("", {});
        const auto remote = makeDir("", {makeFile("Readme", 1, 100), makeFile("readme", 1, 100)});
        {
            CollectingSink sink;
            diffScanTrees(local, remote, {.direction = Direction::Download}, sink);
            EXPECT_EQ(sink.downloads.size(), 2u) << "a case sensitive filesystem stores both";
            EXPECT_TRUE(sink.blocked.empty());
        }
        {
            CollectingSink sink;
            diffScanTrees(local, remote, {.direction = Direction::Download, .localCaseInsensitive = true}, sink);
            EXPECT_TRUE(sink.downloads.empty());
            EXPECT_EQ(sink.blocked.size(), 2u);
        }
    }

    TEST(SyncDiffTests, UnreadableLocalDirectoryNeverDeletesItsRemoteContent)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeUnreadableDir("data")}),
            makeDir("", {makeDir("data", {makeFile("a.txt", 1, 100), makeDir("deep", {makeFile("b", 1, 100)})})}),
            {.direction = Direction::Upload, .actionDelete = true},
            sink
        );
        EXPECT_TRUE(sink.deletes.empty());
        EXPECT_TRUE(sink.uploads.empty());
        ASSERT_EQ(sink.blocked.size(), 1u);
        EXPECT_EQ(sink.blocked.front().relKey, "data");
        EXPECT_EQ(sink.blocked.front().reason, BlockReason::Unreadable);
    }

    TEST(SyncDiffTests, UnreadableRemoteDirectoryNeverDeletesItsLocalContent)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeDir("data", {makeFile("a.txt", 1, 100)})}),
            makeDir("", {makeUnreadableDir("data")}),
            {.direction = Direction::Download, .actionDelete = true},
            sink
        );
        EXPECT_TRUE(sink.deletes.empty());
        EXPECT_TRUE(sink.downloads.empty());
        ASSERT_EQ(sink.blocked.size(), 1u);
        EXPECT_EQ(sink.blocked.front().reason, BlockReason::Unreadable);
    }

    TEST(SyncDiffTests, OneSidedUnreadableDirectoryIsNeitherTransferredNorDeleted)
    {
        for (const auto direction : {Direction::Upload, Direction::Download, Direction::Both})
        {
            CollectingSink sink;
            diffScanTrees(
                makeDir("", {makeUnreadableDir("local-only")}),
                makeDir("", {makeUnreadableDir("remote-only")}),
                {.direction = direction, .actionDelete = true},
                sink
            );
            EXPECT_TRUE(sink.uploads.empty());
            EXPECT_TRUE(sink.downloads.empty());
            EXPECT_TRUE(sink.deletes.empty());
            EXPECT_EQ(sink.blocked.size(), 2u);
        }
    }

    TEST(SyncDiffTests, ExcludedLocalEntryKeepsTheRemoteDirectoryOfTheSameName)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeExcluded("assets", FileType::Unknown)}),
            makeDir("", {makeDir("assets", {makeFile("logo.png", 10, 100)})}),
            {.direction = Direction::Upload, .actionDelete = true},
            sink
        );
        EXPECT_TRUE(sink.deletes.empty());
        EXPECT_TRUE(sink.uploads.empty());
        ASSERT_EQ(sink.blocked.size(), 1u);
        EXPECT_EQ(sink.blocked.front().reason, BlockReason::Excluded);
    }

    TEST(SyncDiffTests, OneSidedExcludedEntryIsNotTransferred)
    {
        CollectingSink sink;
        diffScanTrees(
            makeDir("", {makeExcluded("pipe")}),
            makeDir("", {makeExcluded("socket", FileType::Socket)}),
            {.direction = Direction::Both},
            sink
        );
        EXPECT_TRUE(sink.uploads.empty());
        EXPECT_TRUE(sink.downloads.empty());
        EXPECT_EQ(sink.blocked.size(), 2u);
    }

    TEST(SyncDiffTests, DirectoryAgainstSymlinkIsBlockedInBothDirections)
    {
        for (const auto direction : {Direction::Upload, Direction::Download, Direction::Both})
        {
            CollectingSink sink;
            diffScanTrees(
                makeDir("", {makeSymlink("x", "/etc", 200), makeDir("y", {makeFile("a", 1, 100)})}),
                makeDir("", {makeDir("x", {makeFile("passwd", 1, 100)}), makeSymlink("y", "/root", 200)}),
                {.direction = direction, .actionDelete = true},
                sink
            );
            EXPECT_TRUE(sink.uploads.empty());
            EXPECT_TRUE(sink.downloads.empty());
            EXPECT_TRUE(sink.deletes.empty());
            ASSERT_EQ(sink.blocked.size(), 2u);
            EXPECT_EQ(sink.blocked[0].reason, BlockReason::TypeConflict);
            EXPECT_EQ(sink.blocked[1].reason, BlockReason::TypeConflict);
        }
    }
}
