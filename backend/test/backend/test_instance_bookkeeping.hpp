#pragma once

#include <backend/file_tracking/instance_bookkeeping.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <paths/native.hpp>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class InstanceBookkeepingTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            instanceDirectory_ = temporaryRoot / "instance_bookkeeping" / info->name() / "0123-instance";
            std::filesystem::remove_all(instanceDirectory_);
            std::filesystem::create_directories(instanceDirectory_);
        }

        void TearDown() override
        {
            std::filesystem::remove_all(instanceDirectory_.parent_path());
        }

        static void writeText(std::filesystem::path const& path, std::string const& text)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream{path} << text;
        }

        static nlohmann::json readJson(std::filesystem::path const& path)
        {
            std::ifstream input{path};
            return nlohmann::json::parse(input, nullptr, false);
        }

        std::filesystem::path instanceDirectory_;
    };

    TEST_F(InstanceBookkeepingTest, CurrentPathsLieInTheHiddenDirectory)
    {
        const auto paths = FileTracking::bookkeepingPaths(instanceDirectory_);
        const auto hidden = instanceDirectory_ / std::filesystem::path{FileTracking::bookkeepingDirectoryName};
        EXPECT_EQ(paths.lock.parent_path(), hidden);
        EXPECT_EQ(paths.metadata.parent_path(), hidden);
        EXPECT_TRUE(Paths::nativeToUtf8(hidden.filename()).starts_with("."));
    }

    TEST_F(InstanceBookkeepingTest, LegacyPathsLieNextToTheContent)
    {
        const auto paths = FileTracking::legacyBookkeepingPaths(instanceDirectory_);
        EXPECT_EQ(paths.lock, instanceDirectory_ / "0123-instance.lock");
        EXPECT_EQ(paths.metadata, instanceDirectory_ / "metadata.json");
    }

    TEST_F(InstanceBookkeepingTest, BookkeepingPathRecognition)
    {
        const auto hidden = instanceDirectory_ / std::filesystem::path{FileTracking::bookkeepingDirectoryName};
        EXPECT_TRUE(FileTracking::isBookkeepingPath(instanceDirectory_, hidden));
        EXPECT_TRUE(FileTracking::isBookkeepingPath(instanceDirectory_, hidden / "metadata.json"));
        EXPECT_FALSE(FileTracking::isBookkeepingPath(instanceDirectory_, instanceDirectory_));
        EXPECT_FALSE(FileTracking::isBookkeepingPath(instanceDirectory_, instanceDirectory_ / "metadata.json"));
        EXPECT_FALSE(FileTracking::isBookkeepingPath(
            instanceDirectory_, instanceDirectory_ / "sub" / std::filesystem::path{FileTracking::bookkeepingDirectoryName}
        ));
        EXPECT_FALSE(FileTracking::isBookkeepingPath(instanceDirectory_, instanceDirectory_.parent_path()));
    }

    TEST_F(InstanceBookkeepingTest, NoLockMeansNoInstance)
    {
        EXPECT_FALSE(FileTracking::existingLockFile(instanceDirectory_).has_value());
    }

    TEST_F(InstanceBookkeepingTest, LegacyLockIsFoundWhenTheCurrentOneIsMissing)
    {
        const auto legacy = FileTracking::legacyBookkeepingPaths(instanceDirectory_).lock;
        writeText(legacy, "");
        EXPECT_EQ(FileTracking::existingLockFile(instanceDirectory_), legacy);
    }

    TEST_F(InstanceBookkeepingTest, CurrentLockWinsOverTheLegacyOne)
    {
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).lock, "");
        const auto current = FileTracking::bookkeepingPaths(instanceDirectory_).lock;
        writeText(current, "");
        EXPECT_EQ(FileTracking::existingLockFile(instanceDirectory_), current);
    }

    TEST_F(InstanceBookkeepingTest, MissingMetadataReadsAsNothing)
    {
        const auto metadata = FileTracking::readMetadata(instanceDirectory_);
        ASSERT_TRUE(metadata.has_value());
        EXPECT_FALSE(metadata->has_value());
    }

    TEST_F(InstanceBookkeepingTest, LegacyMetadataIsReadWhenTheCurrentOneIsMissing)
    {
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).lock, "");
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata, R"({"deadAt": 5})");
        const auto metadata = FileTracking::readMetadata(instanceDirectory_);
        ASSERT_TRUE(metadata.has_value() && metadata->has_value());
        EXPECT_EQ((**metadata)["deadAt"], 5);
    }

    TEST_F(InstanceBookkeepingTest, CurrentMetadataWinsOverTheLegacyOne)
    {
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata, R"({"deadAt": 5})");
        writeText(FileTracking::bookkeepingPaths(instanceDirectory_).metadata, R"({"deadAt": 7})");
        const auto metadata = FileTracking::readMetadata(instanceDirectory_);
        ASSERT_TRUE(metadata.has_value() && metadata->has_value());
        EXPECT_EQ((**metadata)["deadAt"], 7);
    }

    TEST_F(InstanceBookkeepingTest, BrokenMetadataIsAnError)
    {
        writeText(FileTracking::bookkeepingPaths(instanceDirectory_).metadata, "{not json");
        EXPECT_FALSE(FileTracking::readMetadata(instanceDirectory_).has_value());
    }

    TEST_F(InstanceBookkeepingTest, WriteCreatesTheHiddenDirectoryAndNoLegacyFile)
    {
        ASSERT_TRUE(FileTracking::writeMetadata(instanceDirectory_, {{"deadAt", 3}}).has_value());
        EXPECT_EQ(readJson(FileTracking::bookkeepingPaths(instanceDirectory_).metadata)["deadAt"], 3);
        EXPECT_FALSE(std::filesystem::exists(FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata));
    }

    TEST_F(InstanceBookkeepingTest, ContentNamedMetadataIsNoMetadata)
    {
        writeText(FileTracking::bookkeepingPaths(instanceDirectory_).lock, "");
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata, R"({"deadAt": 5})");
        const auto metadata = FileTracking::readMetadata(instanceDirectory_);
        ASSERT_TRUE(metadata.has_value());
        EXPECT_FALSE(metadata->has_value());
    }

    TEST_F(InstanceBookkeepingTest, WriteLeavesContentNamedMetadataAlone)
    {
        writeText(FileTracking::bookkeepingPaths(instanceDirectory_).lock, "");
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).lock, "content");
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata, "content");
        ASSERT_TRUE(FileTracking::writeMetadata(instanceDirectory_, {{"deadAt", 9}}).has_value());
        std::ifstream input{FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata};
        const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        EXPECT_EQ(content, "content");
    }

    TEST_F(InstanceBookkeepingTest, WriteMirrorsIntoTheMetadataOfALegacyInstance)
    {
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).lock, "");
        writeText(FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata, R"({"deadAt": null})");
        ASSERT_TRUE(FileTracking::writeMetadata(instanceDirectory_, {{"deadAt", 9}}).has_value());
        EXPECT_EQ(readJson(FileTracking::bookkeepingPaths(instanceDirectory_).metadata)["deadAt"], 9);
        EXPECT_EQ(readJson(FileTracking::legacyBookkeepingPaths(instanceDirectory_).metadata)["deadAt"], 9);
    }

    TEST_F(InstanceBookkeepingTest, FileStatesListFilesAndDirectoriesWithoutBookkeeping)
    {
        ASSERT_TRUE(FileTracking::createBookkeepingDirectory(instanceDirectory_).has_value());
        writeText(FileTracking::bookkeepingPaths(instanceDirectory_).metadata, "{}");
        writeText(instanceDirectory_ / "metadata.json", "tracked");
        writeText(instanceDirectory_ / "tree" / "inner.txt", "12345");

        const auto states = FileTracking::InstanceManagerDetail::fileStatesOf(instanceDirectory_);
        ASSERT_TRUE(states.is_array());
        const auto find = [&states](std::filesystem::path const& path) -> nlohmann::json
        {
            const auto wanted = Paths::nativeToUtf8(std::filesystem::weakly_canonical(path));
            const auto found = std::ranges::find_if(
                states,
                [&wanted](nlohmann::json const& state)
                {
                    return state["path"] == wanted;
                }
            );
            return found == states.end() ? nlohmann::json{} : *found;
        };
        EXPECT_EQ(states.size(), 3u);
        EXPECT_EQ(find(instanceDirectory_ / "metadata.json")["size"], 7);
        EXPECT_EQ(find(instanceDirectory_ / "tree" / "inner.txt")["size"], 5);
        EXPECT_TRUE(find(instanceDirectory_ / "tree" / "inner.txt").contains("mtime"));
        EXPECT_EQ(find(instanceDirectory_ / "tree")["directory"], true);
        EXPECT_TRUE(find(FileTracking::bookkeepingPaths(instanceDirectory_).metadata).is_null());
    }
}
