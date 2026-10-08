#include <command-store/snippet_import_plan.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    using CommandStore::Snippet;
    using CommandStore::SnippetFolder;
    using CommandStore::TransferSnippet;
    using CommandStore::SnippetImport::Resolution;
    using CommandStore::SnippetImport::Status;
    using CommandStore::SnippetImport::Target;
    using CommandStore::SnippetImport::decide;
    using CommandStore::SnippetImport::plan;

    class SnippetImportPlanTests : public ::testing::Test
    {
      protected:
        std::vector<SnippetFolder> folders_{
            {.id = "f-files", .name = "Files", .position = 0},
            {.id = "f-net", .name = "Network", .position = 1},
        };
        std::vector<Snippet> stored_{
            {.id = "s-list", .name = "List", .command = "ls", .folder = "f-files", .tags = {"quick"}},
            {.id = "s-ping", .name = "Ping", .command = "ping {{host}}", .folder = "f-net"},
            {.id = "s-root", .name = "Uptime", .command = "uptime"},
        };
    };

    TEST_F(SnippetImportPlanTests, DocumentTargetResolvesFoldersByNameAndListsNewOnes)
    {
        const auto result = plan(
            {
                {.name = "Df", .command = "df -h", .folder = "Files"},
                {.name = "Top", .command = "top", .folder = "System"},
                {.name = "Free", .command = "free", .folder = "System"},
                {.name = "Who", .command = "who"},
            },
            stored_,
            folders_,
            Target{}
        );
        ASSERT_EQ(result.snippets.size(), 4u);
        EXPECT_EQ(result.snippets[0].folderId, "f-files");
        EXPECT_FALSE(result.snippets[0].createsFolder);
        EXPECT_TRUE(result.snippets[1].createsFolder);
        EXPECT_EQ(result.snippets[1].folderId, "");
        EXPECT_EQ(result.snippets[3].folderId, "");
        EXPECT_FALSE(result.snippets[3].createsFolder);
        EXPECT_EQ(result.newFolders, (std::vector<std::string>{"System"}));
        EXPECT_TRUE(result.ignoredFolders.empty());
        EXPECT_EQ(result.count(Status::New), 4u);
    }

    TEST_F(SnippetImportPlanTests, FixedTargetIgnoresTheDocumentFolders)
    {
        const auto result = plan(
            {
                {.name = "Df", .command = "df -h", .folder = "Files"},
                {.name = "Top", .command = "top", .folder = "System"},
                {.name = "Who", .command = "who"},
            },
            stored_,
            folders_,
            Target{.folderId = "f-net"}
        );
        for (auto const& planned : result.snippets)
        {
            EXPECT_EQ(planned.folderId, "f-net");
            EXPECT_EQ(planned.snippet.folder, "Network");
            EXPECT_FALSE(planned.createsFolder);
        }
        EXPECT_TRUE(result.newFolders.empty());
        EXPECT_EQ(result.ignoredFolders, (std::vector<std::string>{"Files", "System"}));
    }

    TEST_F(SnippetImportPlanTests, RootTargetComparesAgainstUnfiledSnippets)
    {
        const auto result = plan(
            {{.name = "Uptime", .command = "uptime", .folder = "Files"}}, stored_, folders_, Target{.folderId = ""}
        );
        ASSERT_EQ(result.snippets.size(), 1u);
        EXPECT_EQ(result.snippets[0].folderId, "");
        EXPECT_EQ(result.snippets[0].snippet.folder, "");
        EXPECT_EQ(result.snippets[0].status, Status::Identical);
    }

    TEST_F(SnippetImportPlanTests, SameNameCommandAndTagsIsIdenticalRegardlessOfFavorite)
    {
        const auto result = plan(
            {{.name = "List", .command = "ls", .folder = "Files", .tags = {"quick"}, .favorite = true}},
            stored_,
            folders_,
            Target{}
        );
        ASSERT_EQ(result.snippets.size(), 1u);
        EXPECT_EQ(result.snippets[0].status, Status::Identical);
        ASSERT_TRUE(result.snippets[0].existing.has_value());
        EXPECT_EQ(result.snippets[0].existing->id, "s-list");
    }

    TEST_F(SnippetImportPlanTests, SameNameWithOtherContentIsAConflict)
    {
        const auto result = plan(
            {
                {.name = "List", .command = "ls -la", .folder = "Files", .tags = {"quick"}},
                {.name = "List", .command = "ls", .folder = "Files"},
            },
            stored_,
            folders_,
            Target{}
        );
        ASSERT_EQ(result.snippets.size(), 2u);
        for (auto const& planned : result.snippets)
        {
            EXPECT_EQ(planned.status, Status::NameConflict);
            ASSERT_TRUE(planned.existing.has_value());
            EXPECT_EQ(planned.existing->id, "s-list");
        }
        // Only the first may overwrite the stored snippet, or the second would silently undo it.
        EXPECT_TRUE(result.snippets[0].canReplace());
        EXPECT_FALSE(result.snippets[1].canReplace());
    }

    TEST_F(SnippetImportPlanTests, SameNameInAnotherFolderIsNew)
    {
        const auto result =
            plan({{.name = "List", .command = "ls -la", .folder = "Network"}}, stored_, folders_, Target{});
        ASSERT_EQ(result.snippets.size(), 1u);
        EXPECT_EQ(result.snippets[0].status, Status::New);
        EXPECT_FALSE(result.snippets[0].existing.has_value());
    }

    TEST_F(SnippetImportPlanTests, DuplicatesWithinTheDocumentAreDetected)
    {
        const auto result = plan(
            {
                {.name = "Top", .command = "top", .folder = "System"},
                {.name = "Top", .command = "top", .folder = "System"},
                {.name = "Top", .command = "htop", .folder = "System"},
                {.name = "Top", .command = "htop", .folder = "Other"},
            },
            stored_,
            folders_,
            Target{}
        );
        ASSERT_EQ(result.snippets.size(), 4u);
        EXPECT_EQ(result.snippets[0].status, Status::New);
        EXPECT_EQ(result.snippets[1].status, Status::Identical);
        EXPECT_EQ(result.snippets[1].earlierEntry, 0u);
        EXPECT_EQ(result.snippets[2].status, Status::NameConflict);
        EXPECT_EQ(result.snippets[2].earlierEntry, 0u);
        EXPECT_FALSE(result.snippets[2].canReplace());
        EXPECT_EQ(result.snippets[3].status, Status::New);
    }

    TEST_F(SnippetImportPlanTests, DecideFollowsTheResolutions)
    {
        const auto result = plan(
            {
                {.name = "Df", .command = "df -h", .folder = "Files"},
                {.name = "List", .command = "ls", .folder = "Files", .tags = {"quick"}},
                {.name = "List", .command = "ls -1", .folder = "Files"},
                {.name = "Ping", .command = "ping -c 3 {{host}}", .folder = "Network"},
                {.name = "Top", .command = "top", .folder = "System"},
                {.name = "Top", .command = "htop", .folder = "System"},
            },
            stored_,
            folders_,
            Target{}
        );
        const auto decision = decide(
            result,
            [](std::size_t index)
            {
                switch (index)
                {
                    case 2:
                        return Resolution::KeepBoth;
                    case 3:
                        return Resolution::Replace;
                    case 5:
                        return Resolution::Replace;
                    default:
                        return Resolution::Skip;
                }
            }
        );

        // Df new, List identical skipped, List kept, Ping replaced, Top new, second Top cannot replace.
        EXPECT_EQ(decision.skipped, 2u);
        ASSERT_EQ(decision.entries.size(), 4u);
        EXPECT_EQ(decision.entries[0].snippet.name, "Df");
        EXPECT_EQ(decision.entries[0].folderId, "f-files");
        EXPECT_EQ(decision.entries[1].snippet.command, "ls -1");
        EXPECT_TRUE(decision.entries[1].replaces.empty());
        EXPECT_EQ(decision.entries[2].replaces, "s-ping");
        EXPECT_EQ(decision.entries[3].snippet.folder, "System");
        EXPECT_EQ(decision.entries[3].folderId, "");
    }

    TEST_F(SnippetImportPlanTests, DecideMergesFavoritesQuietly)
    {
        auto stored = stored_;
        stored[1].favorite = true;
        const auto result = plan(
            {
                {.name = "List", .command = "ls", .folder = "Files", .tags = {"quick"}, .favorite = true},
                {.name = "Ping", .command = "ping -c 3 {{host}}", .folder = "Network"},
                {.name = "Top", .command = "top", .folder = "System"},
                {.name = "Top", .command = "top", .folder = "System", .favorite = true},
            },
            stored,
            folders_,
            Target{}
        );
        const auto decision = decide(
            result,
            [](std::size_t)
            {
                return Resolution::Replace;
            }
        );

        // List only gains the flag, Ping keeps it through the replace, Top takes it from its duplicate.
        EXPECT_EQ(decision.added, 1u);
        EXPECT_EQ(decision.replaced, 1u);
        EXPECT_EQ(decision.skipped, 2u);
        ASSERT_EQ(decision.entries.size(), 3u);
        EXPECT_EQ(decision.entries[0].replaces, "s-list");
        EXPECT_TRUE(decision.entries[0].snippet.favorite);
        EXPECT_EQ(decision.entries[1].replaces, "s-ping");
        EXPECT_TRUE(decision.entries[1].snippet.favorite);
        EXPECT_EQ(decision.entries[2].snippet.name, "Top");
        EXPECT_TRUE(decision.entries[2].snippet.favorite);
    }

    TEST_F(SnippetImportPlanTests, SkippedConflictLeavesTheStoredSnippetUntouched)
    {
        const auto result = plan(
            {
                {.name = "List", .command = "ls -la", .folder = "Files", .favorite = true},
                {.name = "List", .command = "ls -1", .folder = "Files", .favorite = true},
            },
            stored_,
            folders_,
            Target{}
        );
        const auto decision = decide(
            result,
            [](std::size_t)
            {
                return Resolution::Skip;
            }
        );

        // A different command under the same name is no reason to star the stored snippet.
        EXPECT_EQ(decision.skipped, 2u);
        EXPECT_TRUE(decision.entries.empty());
    }

    TEST_F(SnippetImportPlanTests, IdenticalFavoriteMarksTheStoredSnippetOnce)
    {
        const auto result = plan(
            {
                {.name = "List", .command = "ls", .folder = "Files", .tags = {"quick"}, .favorite = true},
                {.name = "List", .command = "ls", .folder = "Files", .tags = {"quick"}, .favorite = true},
            },
            stored_,
            folders_,
            Target{}
        );
        const auto decision = decide(
            result,
            [](std::size_t)
            {
                return Resolution::Skip;
            }
        );

        // The stored command and tags stay, only the flag is merged, and only once.
        EXPECT_EQ(decision.added, 0u);
        EXPECT_EQ(decision.replaced, 0u);
        EXPECT_EQ(decision.skipped, 2u);
        ASSERT_EQ(decision.entries.size(), 1u);
        EXPECT_EQ(decision.entries[0].replaces, "s-list");
        EXPECT_EQ(decision.entries[0].folderId, "f-files");
        EXPECT_EQ(decision.entries[0].snippet.command, "ls");
        EXPECT_EQ(decision.entries[0].snippet.tags, (std::vector<std::string>{"quick"}));
        EXPECT_TRUE(decision.entries[0].snippet.favorite);
    }

    TEST_F(SnippetImportPlanTests, DecideNeverUnfavorites)
    {
        auto stored = stored_;
        stored[0].favorite = true;
        const auto result = plan(
            {{.name = "List", .command = "ls", .folder = "Files", .tags = {"quick"}}}, stored, folders_, Target{}
        );
        const auto decision = decide(
            result,
            [](std::size_t)
            {
                return Resolution::Replace;
            }
        );
        EXPECT_TRUE(decision.entries.empty());
        EXPECT_EQ(decision.skipped, 1u);
    }
}
