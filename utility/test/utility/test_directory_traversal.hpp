#pragma once

#include <shared_data/directory_entry.hpp>
#include <utility/directory_traversal.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <fstream>
#include <random>

using namespace std::string_literals;

extern std::filesystem::path programDirectory;

namespace Utility::Test
{
    class DirectoryTraversalTests : public ::testing::Test
    {
      public:
        template <bool IncludesDotAndDotDot = false>
        auto withWalkerDo(auto&& fn)
        {
            auto scan = [this](std::filesystem::path const& path)
                -> std::expected<std::vector<SharedData::DirectoryEntry>, std::string> {
                if (scanner_)
                    return scanner_(path);
                else
                    return std::unexpected("No Scanner Installed In Test"s);
            };
            using WalkerType =
                DeepDirectoryWalker<SharedData::DirectoryEntry, std::string, decltype(scan), IncludesDotAndDotDot>;
            if (!walker_)
                walker_ = std::make_unique<WalkerType>(isolateDirectory_.path(), std::move(scan));
            return fn(static_cast<WalkerType&>(*walker_));
        }

        std::filesystem::path createFileIn(std::filesystem::path const& dir, std::size_t size)
        {
            const auto path = dir / ("file" + std::to_string(fileCounter_++));
            std::ofstream out{path, std::ios_base::binary};
            for (std::size_t i = 0; i < size; ++i)
                out.put('A' + (i % 26));
            return path;
        }

        std::vector<std::filesystem::path> createSomeFilesIn(std::filesystem::path const& dir, std::size_t count)
        {
            std::vector<std::filesystem::path> paths{};
            for (std::size_t i = 0; i < count; ++i)
                paths.push_back(createFileIn(dir, fileCounter_ + 1));
            return paths;
        }

        std::filesystem::path createDirectoryIn(std::filesystem::path const& parent)
        {
            const auto path = parent / ("dir" + std::to_string(dirCounter_++));
            std::filesystem::create_directory(path);
            return path;
        }

        std::vector<std::filesystem::path>
        createSomeDirectoriesIn(std::filesystem::path const& parent, std::size_t count)
        {
            std::vector<std::filesystem::path> dirs{};
            for (std::size_t i = 0; i < count; ++i)
                dirs.push_back(createDirectoryIn(parent));
            return dirs;
        }

        void makeDefaultScan()
        {
            scanner_ = [](std::filesystem::path const& path)
                -> std::expected<std::vector<SharedData::DirectoryEntry>, std::string> {
                std::vector<SharedData::DirectoryEntry> entries{};
                for (auto const& dirEntry : std::filesystem::directory_iterator(path))
                {
                    SharedData::DirectoryEntry entry{};
                    entry.path = dirEntry.path().filename();
                    entry.type =
                        dirEntry.is_directory() ? SharedData::FileType::Directory : SharedData::FileType::Regular;
                    entry.size = dirEntry.is_regular_file() ? dirEntry.file_size() : 0;
                    entries.push_back(std::move(entry));
                }
                return entries;
            };
        }

      protected:
        Utility::TemporaryDirectory isolateDirectory_{programDirectory / "temp", true};
        std::unique_ptr<BaseDirectoryWalker> walker_;
        std::function<std::expected<std::vector<SharedData::DirectoryEntry>, std::string>(std::filesystem::path const&)>
            scanner_;
        int fileCounter_{0};
        int dirCounter_{0};
    };

    TEST_F(DirectoryTraversalTests, CanTraverseEmptyDirectory)
    {
        makeDefaultScan();
        const auto result = withWalkerDo([](auto& walker) {
            const auto res = walker.walk();
            return std::make_tuple(
                res, walker.totalEntries(), walker.totalBytes(), walker.currentIndex(), walker.completed());
        });

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_TRUE(std::get<0>(result).value()) << "Walk should be complete immediately";
        EXPECT_EQ(std::get<1>(result), 1) << "Total entries should be one (root)";
        EXPECT_EQ(std::get<2>(result), 0) << "Total bytes should be zero";
        EXPECT_EQ(std::get<3>(result), 1) << "Current index should be one";
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, SingleFileIsFound)
    {
        makeDefaultScan();
        createFileIn(isolateDirectory_.path(), 1234);

        auto result = withWalkerDo([](auto& walker) {
            const auto res = walker.walk();
            return std::make_tuple(
                res, walker.totalEntries(), walker.totalBytes(), walker.currentIndex(), walker.completed());
        });

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_TRUE(std::get<0>(result).value()) << "Walk should have no more work";
        EXPECT_EQ(std::get<1>(result), 2) << "Total entries should be 2";
        EXPECT_EQ(std::get<2>(result), 1234) << "Total bytes should be 1234";
        EXPECT_EQ(std::get<3>(result), 2) << "Current index should be 2";
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, MultipleFilesAreFound)
    {
        constexpr auto n = 10;

        makeDefaultScan();
        createSomeFilesIn(isolateDirectory_.path(), 10);

        auto result = withWalkerDo([](auto& walker) {
            const auto res = walker.walk();
            return std::make_tuple(
                res, walker.totalEntries(), walker.totalBytes(), walker.currentIndex(), walker.completed());
        });

        constexpr auto calculatedSize = (n * (n + 1)) / 2;

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_TRUE(std::get<0>(result).value()) << "Walk should have no more work";
        EXPECT_EQ(std::get<1>(result), 11) << "Total entries should be 11";
        EXPECT_EQ(std::get<2>(result), calculatedSize) << "Total bytes should be " << calculatedSize;
        EXPECT_EQ(std::get<3>(result), 11) << "Current index should be 11";
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, DirectoriesAreFound)
    {
        constexpr auto n = 5;

        makeDefaultScan();
        createSomeDirectoriesIn(isolateDirectory_.path(), n);

        auto result = withWalkerDo([](auto& walker) {
            const auto res = walker.walkAll();
            return std::make_tuple(
                res, walker.totalEntries(), walker.totalBytes(), walker.currentIndex(), walker.completed());
        });

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_EQ(std::get<1>(result), n + 1) << "Total entries should be " << n + 1;
        EXPECT_EQ(std::get<2>(result), 0) << "Total bytes should be 0";
        EXPECT_EQ(std::get<3>(result), n + 1) << "Current index should be " << n + 1;
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, AMixOfFilesAndDirectoriesIsFound)
    {
        constexpr auto dirCount = 3;
        constexpr auto fileCount = 7;

        makeDefaultScan();
        createSomeDirectoriesIn(isolateDirectory_.path(), dirCount);
        createSomeFilesIn(isolateDirectory_.path(), fileCount);

        auto result = withWalkerDo([](auto& walker) {
            const auto res = walker.walkAll();
            return std::make_tuple(
                res, walker.totalEntries(), walker.totalBytes(), walker.currentIndex(), walker.completed());
        });

        constexpr auto calculatedSize = (fileCount * (fileCount + 1)) / 2;

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_EQ(std::get<1>(result), dirCount + fileCount + 1)
            << "Total entries should be " << (dirCount + fileCount + 1);
        EXPECT_EQ(std::get<2>(result), calculatedSize) << "Total bytes should be " << calculatedSize;
        EXPECT_EQ(std::get<3>(result), dirCount + fileCount + 1)
            << "Current index should be " << (dirCount + fileCount + 1);
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, FilesInsideSingleDirectoryAreFound)
    {
        constexpr auto dirCount = 1;
        constexpr auto fileCount = 5;

        makeDefaultScan();
        createSomeDirectoriesIn(isolateDirectory_.path(), dirCount);
        const auto subDir = isolateDirectory_.path() / "dir0";
        createSomeFilesIn(subDir, fileCount);

        auto result = withWalkerDo([](auto& walker) {
            auto res = walker.walkAll();
            return std::make_tuple(
                std::move(res),
                walker.totalEntries(),
                walker.totalBytes(),
                walker.currentIndex(),
                walker.completed());
        });

        constexpr auto calculatedSize = (fileCount * (fileCount + 1)) / 2;

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_EQ(std::get<1>(result), dirCount + fileCount + 1)
            << "Total entries should be " << (dirCount + fileCount + 1);
        EXPECT_EQ(std::get<2>(result), calculatedSize) << "Total bytes should be " << calculatedSize;
        EXPECT_EQ(std::get<3>(result), dirCount + fileCount + 1)
            << "Current index should be " << (dirCount + fileCount + 1);
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, DeeplyNestedFilesAreFound)
    {
        constexpr auto depth = 4;
        constexpr auto fileCountPerLevel = 3;

        makeDefaultScan();
        auto currentDir = isolateDirectory_.path();

        for (std::size_t i = 0; i < depth; ++i)
        {
            createSomeFilesIn(currentDir, fileCountPerLevel);
            currentDir = createDirectoryIn(currentDir);
        }

        auto result = withWalkerDo([](auto& walker) {
            auto res = walker.walkAll();
            return std::make_tuple(
                std::move(res),
                walker.totalEntries(),
                walker.totalBytes(),
                walker.currentIndex(),
                walker.completed());
        });

        constexpr auto totalFiles = depth * fileCountPerLevel;
        const auto calculatedSize = ((fileCounter_) * (fileCounter_ + 1) / 2);
        constexpr auto totalEntries = totalFiles + depth + 1;

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_EQ(std::get<1>(result), totalEntries) << "Total entries should be " << totalEntries;
        EXPECT_EQ(std::get<2>(result), calculatedSize) << "Total bytes should be " << calculatedSize;
        EXPECT_EQ(std::get<3>(result), totalEntries) << "Current index should be " << totalEntries;
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, WideAndDeeplyNestedFilesAreFound)
    {
        constexpr auto depth = 3;
        constexpr auto dirCountPerLevel = 3;
        constexpr auto fileCountPerLevel = 2;

        makeDefaultScan();
        auto currentLevelDirs = std::vector<std::filesystem::path>{isolateDirectory_.path()};

        auto createdFiles = 0;
        auto createdDirs = 0;

        for (std::size_t d = 0; d < depth; ++d)
        {
            std::vector<std::filesystem::path> nextLevelDirs{};
            for (auto const& dir : currentLevelDirs)
            {
                createSomeFilesIn(dir, fileCountPerLevel);
                createdFiles += fileCountPerLevel;
                const auto newDirs = createSomeDirectoriesIn(dir, dirCountPerLevel);
                createdDirs += dirCountPerLevel;
                nextLevelDirs.insert(nextLevelDirs.end(), newDirs.begin(), newDirs.end());
            }
            currentLevelDirs = std::move(nextLevelDirs);
        }

        auto result = withWalkerDo([](auto& walker) {
            auto res = walker.walkAll();
            return std::make_tuple(
                std::move(res),
                walker.totalEntries(),
                walker.totalBytes(),
                walker.currentIndex(),
                walker.completed());
        });

        const auto calculatedSize = (createdFiles * (createdFiles + 1) / 2);
        const auto totalEntries = createdFiles + createdDirs + 1;

        ASSERT_TRUE(std::get<0>(result).has_value())
            << "Error during walk: " << (std::get<0>(result).has_value() ? "" : std::get<0>(result).error());
        EXPECT_EQ(std::get<1>(result), totalEntries) << "Total entries should be " << totalEntries;
        EXPECT_EQ(std::get<2>(result), calculatedSize) << "Total bytes should be " << calculatedSize;
        EXPECT_EQ(std::get<3>(result), totalEntries) << "Current index should be " << totalEntries;
        EXPECT_TRUE(std::get<4>(result)) << "There should not be any more work";
    }

    TEST_F(DirectoryTraversalTests, ListedFilesHaveTheExpectedPaths)
    {
        constexpr auto dirCount = 2;
        constexpr auto fileCount = 3;

        makeDefaultScan();
        createSomeFilesIn(isolateDirectory_.path(), fileCount);
        createSomeDirectoriesIn(isolateDirectory_.path(), dirCount);
        const auto subDir = isolateDirectory_.path() / "dir0";
        createSomeFilesIn(subDir, fileCount);
        const auto nestedPaths = createSomeDirectoriesIn(subDir, dirCount);
        const auto subSubDir = nestedPaths[0];
        createSomeFilesIn(subSubDir, fileCount);

        std::vector<std::filesystem::path> paths{};
        const auto walkResult = withWalkerDo([&paths](auto& walker) {
            auto res = walker.walkAll();
            auto const& entries = walker.entries();
            std::transform(entries.begin(), entries.end(), std::back_inserter(paths), [&walker](auto const& entry) {
                return walker.fullPath(entry);
            });
            return res;
        });

        ASSERT_TRUE(walkResult.has_value())
            << "Error during walk: " << (walkResult.has_value() ? "" : walkResult.error());

        std::vector<std::filesystem::path> expectedPaths{
            isolateDirectory_.path(),
            isolateDirectory_.path() / "dir0",
            isolateDirectory_.path() / "dir1",
            isolateDirectory_.path() / "file0",
            isolateDirectory_.path() / "file1",
            isolateDirectory_.path() / "file2",
            isolateDirectory_.path() / "dir0" / "dir2",
            isolateDirectory_.path() / "dir0" / "dir3",
            isolateDirectory_.path() / "dir0" / "file3",
            isolateDirectory_.path() / "dir0" / "file4",
            isolateDirectory_.path() / "dir0" / "file5",
            isolateDirectory_.path() / "dir0" / "dir2" / "file6",
            isolateDirectory_.path() / "dir0" / "dir2" / "file7",
            isolateDirectory_.path() / "dir0" / "dir2" / "file8",
        };

        std::vector<std::pair<std::filesystem::path, bool /* was found */>> expectedPathChecklist{};

        std::transform(
            expectedPaths.begin(),
            expectedPaths.end(),
            std::back_inserter(expectedPathChecklist),
            [](auto const& path) {
                return std::make_pair(path, false);
            });

        for (auto const& foundPath : paths)
        {
            auto it = std::find_if(
                expectedPathChecklist.begin(), expectedPathChecklist.end(), [&foundPath](auto const& pair) {
                    return pair.first == foundPath;
                });
            ASSERT_NE(it, expectedPathChecklist.end()) << "Found unexpected path: " << foundPath;
            if (it != expectedPathChecklist.end())
                it->second = true;
        }

        bool allFound = true;
        for (auto const& [path, wasFound] : expectedPathChecklist)
        {
            if (!wasFound)
            {
                allFound = false;
                break;
            }
        }
        EXPECT_TRUE(allFound) << "Not all expected paths were found";
    }

    TEST_F(DirectoryTraversalTests, DotDirectoriesAreFilteredWhenAtFront)
    {
        scanner_ =
            [](std::filesystem::path const&) -> std::expected<std::vector<SharedData::DirectoryEntry>, std::string> {
            return std::vector<SharedData::DirectoryEntry>{
                SharedData::DirectoryEntry{
                    .path = ".",
                    .type = SharedData::FileType::Directory,
                },
                SharedData::DirectoryEntry{
                    .path = "..",
                    .type = SharedData::FileType::Directory,
                },
                SharedData::DirectoryEntry{
                    .path = "file0",
                    .type = SharedData::FileType::Regular,
                },
                SharedData::DirectoryEntry{
                    .path = "dir0",
                    .type = SharedData::FileType::Directory,
                },
            };
        };

        std::vector<std::filesystem::path> paths{};
        const auto walkResult = withWalkerDo<true>([&paths](auto& walker) {
            auto res = walker.walk();
            auto const& entries = walker.entries();
            std::transform(entries.begin(), entries.end(), std::back_inserter(paths), [&walker](auto const& entry) {
                return walker.fullPath(entry);
            });
            return res;
        });

        ASSERT_TRUE(walkResult.has_value())
            << "Error during walk: " << (walkResult.has_value() ? "" : walkResult.error());

        std::vector<std::filesystem::path> expectedPaths{
            isolateDirectory_.path(),
            isolateDirectory_.path() / "dir0",
            isolateDirectory_.path() / "file0",
        };

        EXPECT_THAT(paths, ::testing::UnorderedElementsAreArray(expectedPaths));
    }

    TEST_F(DirectoryTraversalTests, DotDirectoriesAreFilteredWhenRandomlyPlaced)
    {
        std::vector<SharedData::DirectoryEntry> results{
            SharedData::DirectoryEntry{
                .path = "file0",
                .type = SharedData::FileType::Regular,
            },
            SharedData::DirectoryEntry{
                .path = ".",
                .type = SharedData::FileType::Directory,
            },
            SharedData::DirectoryEntry{
                .path = "dir0",
                .type = SharedData::FileType::Directory,
            },
            SharedData::DirectoryEntry{
                .path = "..",
                .type = SharedData::FileType::Directory,
            },
        };

        std::mt19937 g(0);
        auto shuffle = [&]() {
            std::shuffle(results.begin(), results.end(), g);
        };

        scanner_ =
            [&results](
                std::filesystem::path const&) -> std::expected<std::vector<SharedData::DirectoryEntry>, std::string> {
            return results;
        };

        std::vector<std::filesystem::path> paths{};

        auto doTheWalk = [this, &paths]() {
            paths.clear();
            const auto walkResult = withWalkerDo<true>([&paths](auto& walker) {
                walker.reset();
                auto res = walker.walk();
                auto const& entries = walker.entries();
                std::transform(entries.begin(), entries.end(), std::back_inserter(paths), [&walker](auto const& entry) {
                    return walker.fullPath(entry);
                });
                return res;
            });
            ASSERT_TRUE(walkResult.has_value())
                << "Error during walk: " << (walkResult.has_value() ? "" : walkResult.error());
        };

        std::vector<std::filesystem::path> expectedPaths{
            isolateDirectory_.path(),
            isolateDirectory_.path() / "dir0",
            isolateDirectory_.path() / "file0",
        };

        doTheWalk();
        EXPECT_THAT(paths, ::testing::UnorderedElementsAreArray(expectedPaths));

        shuffle();
        doTheWalk();
        EXPECT_THAT(paths, ::testing::UnorderedElementsAreArray(expectedPaths));

        shuffle();
        doTheWalk();
        EXPECT_THAT(paths, ::testing::UnorderedElementsAreArray(expectedPaths));

        shuffle();
        doTheWalk();
        EXPECT_THAT(paths, ::testing::UnorderedElementsAreArray(expectedPaths));
    }
}