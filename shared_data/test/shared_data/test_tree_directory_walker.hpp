#pragma once

#include <shared_data/sync/tree_directory_walker.hpp>

#include <gtest/gtest.h>

#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace SharedData::Sync::Test
{
    /**
     * @brief In-memory directory listing for the walker, keyed by generic path.
     */
    struct FakeFilesystem
    {
        std::map<std::string, std::vector<DirectoryEntry>> directories{};
        std::vector<std::string> listed{};

        void addFile(std::string const& directory, std::string const& name, std::uint64_t size, std::uint64_t mtime = 1)
        {
            directories[directory].push_back(
                DirectoryEntry{
                    .name = name,
                    .type = FileType::Regular,
                    .size = size,
                    .mtime = mtime,
                }
            );
        }

        void addDirectory(std::string const& directory, std::string const& name)
        {
            directories[directory].push_back(DirectoryEntry{.name = name, .type = FileType::Directory});
            directories[directory + "/" + name];
        }
    };

    inline auto makeScanner(FakeFilesystem& filesystem)
    {
        // The walked root is "/root", the walker names directories relative to it.
        return [&filesystem](Paths::RelativePath const& relative) -> std::expected<std::vector<DirectoryEntry>, std::string>
        {
            const auto key = relative.empty() ? std::string{"/root"} : "/root/" + relative.value();
            filesystem.listed.push_back(key);
            const auto iter = filesystem.directories.find(key);
            if (iter == filesystem.directories.end())
                return std::unexpected(std::string{"missing "} + key);
            return iter->second;
        };
    }

    template <bool IncludesDots = false>
    inline std::expected<ScanNode, std::string> walkFake(FakeFilesystem& filesystem, bool recursive = true)
    {
        auto scanner = makeScanner(filesystem);
        TreeDirectoryWalker<std::string, decltype(scanner), IncludesDots> walker{std::move(scanner), recursive
        };
        auto result = walker.walkAll();
        if (!result)
            return std::unexpected(result.error());
        return std::move(walker).ejectTree();
    }

    inline std::vector<std::string> childNames(ScanNode const& node)
    {
        std::vector<std::string> names;
        for (auto const& child : node.children)
            names.push_back(child.name);
        return names;
    }

    TEST(TreeDirectoryWalkerTests, ChildrenAreSortedByByteOrder)
    {
        FakeFilesystem filesystem;
        filesystem.directories["/root"];
        for (auto const* name : {"b", "a.b", "B", "a", "a-b", "\xc3\xbc"})
            filesystem.addFile("/root", name, 1);

        const auto tree = walkFake(filesystem);
        ASSERT_TRUE(tree.has_value());
        EXPECT_EQ(childNames(*tree), (std::vector<std::string>{"B", "a", "a-b", "a.b", "b", "\xc3\xbc"}));
    }

    TEST(TreeDirectoryWalkerTests, SubtreeMetricsAccumulateBottomUp)
    {
        FakeFilesystem filesystem;
        filesystem.addFile("/root", "top.txt", 10);
        filesystem.addDirectory("/root", "dir");
        filesystem.addFile("/root/dir", "a.txt", 20);
        filesystem.addDirectory("/root/dir", "inner");
        filesystem.addFile("/root/dir/inner", "b.txt", 30);
        filesystem.addDirectory("/root/dir", "empty");

        const auto tree = walkFake(filesystem);
        ASSERT_TRUE(tree.has_value());
        EXPECT_EQ(tree->subtreeFileCount, 3u);
        EXPECT_EQ(tree->subtreeByteTotal, 60u);

        auto const& dir = tree->children.front();
        ASSERT_EQ(dir.name, "dir");
        EXPECT_EQ(dir.subtreeFileCount, 2u);
        EXPECT_EQ(dir.subtreeByteTotal, 50u);
        EXPECT_EQ(childNames(dir), (std::vector<std::string>{"a.txt", "empty", "inner"}));
        EXPECT_TRUE(dir.childrenKnown);
        EXPECT_EQ(dir.children[1].subtreeFileCount, 0u);
    }

    TEST(TreeDirectoryWalkerTests, DotEntriesAreDroppedWhenTheScannerReportsThem)
    {
        FakeFilesystem filesystem;
        filesystem.addDirectory("/root", ".");
        filesystem.addDirectory("/root", "..");
        filesystem.addFile("/root", ".hidden", 1);
        filesystem.addFile("/root", "file", 1);

        const auto tree = walkFake<true>(filesystem);
        ASSERT_TRUE(tree.has_value());
        EXPECT_EQ(childNames(*tree), (std::vector<std::string>{".hidden", "file"}));
    }

    TEST(TreeDirectoryWalkerTests, NonRecursiveListsOnlyTheRootAndMarksDirectoriesUnknown)
    {
        FakeFilesystem filesystem;
        filesystem.addFile("/root", "file", 1);
        filesystem.addDirectory("/root", "dir");
        filesystem.addFile("/root/dir", "nested", 5);

        const auto tree = walkFake(filesystem, /*recursive=*/false);
        ASSERT_TRUE(tree.has_value());
        EXPECT_EQ(filesystem.listed, (std::vector<std::string>{"/root"}));
        ASSERT_EQ(tree->children.size(), 2u);
        auto const& dir = tree->children[0];
        EXPECT_EQ(dir.name, "dir");
        EXPECT_FALSE(dir.childrenKnown);
        EXPECT_TRUE(dir.children.empty());
        EXPECT_TRUE(tree->children[1].childrenKnown) << "files are always fully known";
        EXPECT_TRUE(tree->childrenKnown);
    }

    TEST(TreeDirectoryWalkerTests, RecursiveWalkVisitsEveryDirectoryOnce)
    {
        FakeFilesystem filesystem;
        filesystem.addDirectory("/root", "a");
        filesystem.addDirectory("/root/a", "b");
        filesystem.addDirectory("/root/a/b", "c");
        filesystem.addFile("/root/a/b/c", "deep.txt", 7);

        const auto tree = walkFake(filesystem);
        ASSERT_TRUE(tree.has_value());
        EXPECT_EQ(filesystem.listed, (std::vector<std::string>{"/root", "/root/a", "/root/a/b", "/root/a/b/c"}));
        EXPECT_EQ(tree->subtreeFileCount, 1u);
        EXPECT_EQ(tree->children[0].children[0].children[0].children[0].name, "deep.txt");
    }

    TEST(TreeDirectoryWalkerTests, ScannerErrorsAbortTheWalk)
    {
        FakeFilesystem filesystem;
        filesystem.directories["/root"].push_back(DirectoryEntry{.name = "unlisted", .type = FileType::Directory});

        const auto tree = walkFake(filesystem);
        ASSERT_FALSE(tree.has_value());
        EXPECT_EQ(tree.error(), "missing /root/unlisted");
    }

    TEST(TreeDirectoryWalkerTests, ProgressCountersTrackEntriesAndBytes)
    {
        FakeFilesystem filesystem;
        filesystem.addFile("/root", "a", 100);
        filesystem.addDirectory("/root", "dir");
        filesystem.addFile("/root/dir", "b", 50);

        auto scanner = makeScanner(filesystem);
        TreeDirectoryWalker<std::string, decltype(scanner), false> walker{std::move(scanner)};
        ASSERT_TRUE(walker.walkAll().has_value());
        EXPECT_TRUE(walker.completed());
        EXPECT_EQ(walker.totalEntries(), 4u) << "root, a, dir, b";
        EXPECT_EQ(walker.totalBytes(), 150u);
        EXPECT_EQ(walker.currentIndex(), 2u);
    }
}
