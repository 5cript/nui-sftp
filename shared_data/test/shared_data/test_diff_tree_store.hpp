#pragma once

#include "test_diff.hpp"

#include <shared_data/sync/diff_tree_store.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace SharedData::Sync::Test
{
    /**
     * @brief Runs a diff into three stores and finalizes them.
     */
    struct DiffStores
    {
        ScanNode local;
        ScanNode remote;
        DiffOptions options{};
        DiffTreeStore uploads{};
        DiffTreeStore downloads{};
        DiffTreeStore deletes{};

        DiffStores(ScanNode localTree, ScanNode remoteTree, DiffOptions diffOptions = {})
            : local{std::move(localTree)}
            , remote{std::move(remoteTree)}
            , options{diffOptions}
        {
            uploads.clear(options);
            downloads.clear(options);
            deletes.clear(options);
            StoreDiffSink sink{uploads, downloads, deletes};
            diffScanTrees(local, remote, options, sink);
            uploads.finalize();
            downloads.finalize();
            deletes.finalize();
        }

        ScanTrees trees() const
        {
            return ScanTrees{.local = &local, .remote = &remote, .localRoot = "/l", .remoteRoot = "/r"};
        }
    };

    inline std::vector<std::string> relKeys(std::vector<DiffTreeNode> const& rows)
    {
        std::vector<std::string> keys;
        for (auto const& row : rows)
            keys.push_back(row.relKey);
        return keys;
    }

    inline std::vector<std::string> relKeys(std::vector<EnqueuePlanEntry> const& plan)
    {
        std::vector<std::string> keys;
        for (auto const& entry : plan)
            keys.push_back(entry.relKey);
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    TEST(DiffTreeStoreTests, StructuralAncestorsAreSynthesizedAboveDeepLeaves)
    {
        DiffStores stores{
            makeDir("", {makeDir("a", {makeDir("b", {makeFile("x.txt", 5, 200), makeFile("y.txt", 7, 200)})})}),
            makeDir("", {makeDir("a", {makeDir("b", {})})}),
        };

        auto roots = stores.uploads.loadChildren("", stores.trees());
        ASSERT_EQ(relKeys(roots), (std::vector<std::string>{"a"}));
        EXPECT_TRUE(roots[0].isStructural);
        EXPECT_EQ(roots[0].directChildCount, 1u);
        EXPECT_EQ(roots[0].descendantItemCount, 2u);
        EXPECT_EQ(roots[0].descendantByteTotal, 12u);

        auto middle = stores.uploads.loadChildren("a", stores.trees());
        ASSERT_EQ(relKeys(middle), (std::vector<std::string>{"a/b"}));
        EXPECT_TRUE(middle[0].isStructural);
        EXPECT_EQ(middle[0].directChildCount, 2u);

        EXPECT_EQ(
            relKeys(stores.uploads.loadChildren("a/b", stores.trees())),
            (std::vector<std::string>{"a/b/x.txt", "a/b/y.txt"})
        );
    }

    TEST(DiffTreeStoreTests, ChildrenAreSortedWithStructuralRowsInPlace)
    {
        DiffStores stores{
            makeDir(
                "", {makeFile("z.txt", 1, 200), makeDir("m", {makeFile("deep", 1, 200)}), makeFile("a.txt", 1, 200)}
            ),
            makeDir(
                "", {makeFile("z.txt", 1, 100), makeDir("m", {makeFile("deep", 1, 100)}), makeFile("a.txt", 1, 100)}
            ),
        };
        EXPECT_EQ(
            relKeys(stores.uploads.loadChildren("", stores.trees())), (std::vector<std::string>{"a.txt", "m", "z.txt"})
        );
    }

    TEST(DiffTreeStoreTests, SummaryCountsFilesAndUsesTheTransferredSide)
    {
        DiffStores stores{
            makeDir(
                "",
                {
                    makeDir("new_dir", {makeFile("a", 10, 100), makeFile("b", 20, 100)}),
                    makeDir("empty", {}),
                    makeFile("changed", 1, 100),
                    makeDir("deep", {makeFile("leaf", 3, 300)}),
                }
            ),
            makeDir(
                "",
                {
                    makeFile("changed", 1000, 200),
                    makeDir("deep", {makeFile("leaf", 4, 100)}),
                }
            ),
        };

        auto const& uploads = stores.uploads.summary();
        EXPECT_EQ(uploads.itemCount, 4u) << "two files in new_dir, the empty directory and deep/leaf";
        EXPECT_EQ(uploads.transferBytes, 33u);
        EXPECT_EQ(uploads.rootChildCount, 3u) << "deep, empty, new_dir";

        auto const& downloads = stores.downloads.summary();
        EXPECT_EQ(downloads.itemCount, 1u);
        EXPECT_EQ(downloads.transferBytes, 1000u) << "a download moves the remote bytes";
    }

    TEST(DiffTreeStoreTests, OneSidedDirectoryExpandsLazilyAndCaches)
    {
        DiffStores stores{
            makeDir("", {makeDir("new", {makeFile("a", 1, 100), makeDir("sub", {makeFile("b", 2, 100)})})}),
            makeDir("", {}),
        };
        EXPECT_EQ(stores.uploads.size(), 1u);

        auto children = stores.uploads.loadChildren("new", stores.trees());
        ASSERT_EQ(relKeys(children), (std::vector<std::string>{"new/a", "new/sub"}));
        EXPECT_EQ(children[1].action, Action::Upload);
        EXPECT_TRUE(children[1].isDirectory);
        EXPECT_FALSE(children[1].hasRemoteSide);
        EXPECT_EQ(children[1].directChildCount, 1u);
        EXPECT_EQ(stores.uploads.size(), 3u);

        EXPECT_EQ(
            relKeys(stores.uploads.loadChildren("new/sub", stores.trees())), (std::vector<std::string>{"new/sub/b"})
        );
        stores.uploads.loadChildren("new", stores.trees());
        EXPECT_EQ(stores.uploads.size(), 4u) << "a second load is served from the cache";
    }

    TEST(DiffTreeStoreTests, LazyChildrenOfRemoteDeletesComeFromTheRemoteTree)
    {
        DiffStores stores{
            makeDir("", {}),
            makeDir("", {makeDir("gone", {makeFile("x", 1, 100)})}),
            {.direction = Direction::Upload, .actionDelete = true},
        };
        auto children = stores.deletes.loadChildren("gone", stores.trees());
        ASSERT_EQ(children.size(), 1u);
        EXPECT_EQ(children[0].action, Action::DeleteRemote);
        EXPECT_TRUE(children[0].hasRemoteSide);
    }

    TEST(DiffTreeStoreTests, LazyChildrenHonorIgnoreHidden)
    {
        DiffStores stores{
            makeDir("", {makeDir("new", {makeFile(".secret", 1, 100), makeFile("visible", 1, 100)})}),
            makeDir("", {}),
            {.ignoreHidden = true},
        };
        EXPECT_EQ(
            relKeys(stores.uploads.loadChildren("new", stores.trees())), (std::vector<std::string>{"new/visible"})
        );
    }

    TEST(DiffTreeStoreTests, UnknownParentsHaveNoChildren)
    {
        DiffStores stores{makeDir("", {makeFile("file", 1, 200)}), makeDir("", {makeFile("file", 1, 100)})};
        EXPECT_TRUE(stores.uploads.loadChildren("missing", stores.trees()).empty());
        EXPECT_TRUE(stores.uploads.loadChildren("file", stores.trees()).empty());
        EXPECT_EQ(stores.uploads.size(), 1u);
    }

    TEST(DiffTreeStoreTests, PlanForANewerRemoteFileCarriesRemoteSizeAndMtime)
    {
        // Regression: the plan used the local mtime, the download stamped it on the new file and the next
        // compare listed the file again.
        auto remoteFile = makeFile("socket.cpp", 16, 2000);
        remoteFile.mtimeNsec = 42;
        DiffStores stores{makeDir("", {makeFile("socket.cpp", 9, 1000)}), makeDir("", {remoteFile})};

        const auto plan = stores.downloads.buildPlan({"socket.cpp"}, stores.trees());
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_EQ(plan[0].action, Action::Download);
        EXPECT_EQ(plan[0].sizeBytes, 16u);
        EXPECT_EQ(plan[0].mtime, 2000u);
        EXPECT_EQ(plan[0].mtimeNsec, 42u);
        EXPECT_EQ(plan[0].localAbsPath, "/l/socket.cpp");
        EXPECT_EQ(plan[0].remoteAbsPath, "/r/socket.cpp");
    }

    TEST(DiffTreeStoreTests, PlanForANewerLocalFileCarriesLocalSizeAndMtime)
    {
        DiffStores stores{makeDir("", {makeFile("main.cpp", 21, 2000)}), makeDir("", {makeFile("main.cpp", 12, 1000)})};
        const auto plan = stores.uploads.buildPlan({"main.cpp"}, stores.trees());
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_EQ(plan[0].sizeBytes, 21u);
        EXPECT_EQ(plan[0].mtime, 2000u);
    }

    TEST(DiffTreeStoreTests, PlanExpandsAOneSidedDirectoryIntoItsScannedFiles)
    {
        DiffStores stores{
            makeDir(
                "",
                {makeDir(
                    "module",
                    {
                        makeFile("CMakeLists.txt", 1, 100),
                        makeDir("src", {makeFile("a.cpp", 2, 100), makeDir("detail", {makeFile("b.cpp", 3, 100)})}),
                        makeDir("empty", {}),
                    }
                )}
            ),
            makeDir("", {}),
        };

        const auto plan = stores.uploads.buildPlan({"module"}, stores.trees());
        EXPECT_EQ(
            relKeys(plan),
            (std::vector<std::string>{
                "module/CMakeLists.txt", "module/empty", "module/src/a.cpp", "module/src/detail/b.cpp"
            })
        );
        const auto empty = std::find_if(
            plan.begin(),
            plan.end(),
            [](auto const& entry)
            {
                return entry.relKey == "module/empty";
            }
        );
        EXPECT_TRUE(empty->isDirectory) << "directories without content are created explicitly";
        EXPECT_EQ(
            std::count_if(
                plan.begin(),
                plan.end(),
                [](auto const& entry)
                {
                    return entry.isDirectory;
                }
            ),
            1
        );
    }

    TEST(DiffTreeStoreTests, PlanExpansionSkipsHiddenEntriesWhenIgnoringHidden)
    {
        DiffStores stores{
            makeDir(
                "",
                {makeDir(
                    "module",
                    {makeFile(".env", 1, 100), makeDir(".cache", {makeFile("x", 1, 100)}), makeFile("keep", 1, 100)}
                )}
            ),
            makeDir("", {}),
            {.ignoreHidden = true},
        };
        EXPECT_EQ(
            relKeys(stores.uploads.buildPlan({"module"}, stores.trees())), (std::vector<std::string>{"module/keep"})
        );
    }

    TEST(DiffTreeStoreTests, PlanForAnEmptyOneSidedDirectoryCreatesIt)
    {
        DiffStores stores{makeDir("", {}), makeDir("", {makeDir("empty_remote_dir", {})})};
        const auto plan = stores.downloads.buildPlan({"empty_remote_dir"}, stores.trees());
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_TRUE(plan[0].isDirectory);
        EXPECT_EQ(plan[0].action, Action::Download);
    }

    TEST(DiffTreeStoreTests, PlanForADirectoryWithUnknownContentOnlyCreatesTheDirectory)
    {
        DiffStores stores{makeDir("", {makeDir("unseen", {}, /*childrenKnown=*/false)}), makeDir("", {})};
        const auto plan = stores.uploads.buildPlan({"unseen"}, stores.trees());
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_TRUE(plan[0].isDirectory);
    }

    TEST(DiffTreeStoreTests, PlanKeepsDeletedDirectoriesWhole)
    {
        DiffStores stores{
            makeDir("", {}),
            makeDir("", {makeDir("gone", {makeFile("a", 1, 100), makeFile("b", 1, 100)})}),
            {.direction = Direction::Upload, .actionDelete = true},
        };
        const auto plan = stores.deletes.buildPlan({"gone"}, stores.trees());
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_EQ(plan[0].relKey, "gone");
        EXPECT_TRUE(plan[0].isDirectory);
        EXPECT_EQ(plan[0].action, Action::DeleteRemote);
    }

    TEST(DiffTreeStoreTests, PlanForAPartlySelectedExpandedDirectory)
    {
        DiffStores stores{
            makeDir(
                "",
                {makeDir(
                    "new", {makeFile("a", 1, 100), makeFile("b", 1, 100), makeDir("sub", {makeFile("c", 1, 100)})}
                )}
            ),
            makeDir("", {}),
        };
        stores.uploads.loadChildren("new", stores.trees());

        const auto plan = stores.uploads.buildPlan({"new/a", "new/sub"}, stores.trees());
        EXPECT_EQ(relKeys(plan), (std::vector<std::string>{"new/a", "new/sub/c"}));
    }

    TEST(DiffTreeStoreTests, PlanForAStructuralSelectionCollectsItsLeaves)
    {
        DiffStores stores{
            makeDir("", {makeDir("src", {makeFile("a", 1, 200), makeDir("new", {makeFile("b", 1, 100)})})}),
            makeDir("", {makeDir("src", {makeFile("a", 1, 100)})}),
        };
        EXPECT_EQ(
            relKeys(stores.uploads.buildPlan({"src"}, stores.trees())), (std::vector<std::string>{"src/a", "src/new/b"})
        );
    }

    TEST(DiffTreeStoreTests, TypeConflictDirectoryExpandsFromItsSourceSide)
    {
        auto localDirectory = makeDir("conflict", {makeFile("inside.txt", 7, 300)});
        localDirectory.mtime = 300;
        DiffStores stores{
            makeDir("", {std::move(localDirectory)}),
            makeDir("", {makeFile("conflict", 14, 100)}),
        };
        auto children = stores.uploads.loadChildren("conflict", stores.trees());
        EXPECT_EQ(relKeys(children), (std::vector<std::string>{"conflict/inside.txt"}));
        EXPECT_EQ(
            relKeys(stores.uploads.buildPlan({"conflict"}, stores.trees())),
            (std::vector<std::string>{"conflict/inside.txt"})
        );
    }

    TEST(DiffTreeStoreTests, ClearDropsEverything)
    {
        DiffStores stores{makeDir("", {makeFile("file", 1, 200)}), makeDir("", {})};
        stores.uploads.clear();
        EXPECT_EQ(stores.uploads.size(), 0u);
        EXPECT_EQ(stores.uploads.summary().itemCount, 0u);
        EXPECT_TRUE(stores.uploads.loadChildren("", stores.trees()).empty());
    }

    TEST(DiffTreeStoreTests, FindScanNodeResolvesNestedPaths)
    {
        const auto tree = makeDir("", {makeDir("a", {makeDir("b", {makeFile("c", 1, 1)})}), makeFile("a-b", 1, 1)});
        ASSERT_NE(findScanNode(tree, "a/b/c"), nullptr);
        EXPECT_EQ(findScanNode(tree, "a/b/c")->name, "c");
        EXPECT_EQ(findScanNode(tree, ""), &tree);
        EXPECT_EQ(findScanNode(tree, "a/x"), nullptr);
        EXPECT_EQ(findScanNode(tree, "a-b")->name, "a-b");
    }

    TEST(DiffTreeStoreTests, NonRecursivePlanOnlyCreatesDirectories)
    {
        DiffStores stores{
            makeDir("", {makeDir("new", {makeFile("a", 1, 100)})}),
            makeDir("", {}),
            {.recursive = false},
        };
        EXPECT_TRUE(stores.uploads.loadChildren("new", stores.trees()).empty());
        const auto plan = stores.uploads.buildPlan({"new"}, stores.trees());
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_EQ(plan[0].relKey, "new");
        EXPECT_TRUE(plan[0].isDirectory);
        EXPECT_EQ(stores.uploads.summary().itemCount, 1u);
    }
}
