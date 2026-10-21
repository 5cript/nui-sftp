#pragma once

#include <utility/shared_input_file_stream.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class SharedInputFileStreamTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            content_.reserve(200'000);
            for (std::size_t index = 0; index < 200'000; ++index)
                content_.push_back(static_cast<char>('a' + index % 26));
            std::ofstream{path(), std::ios::binary} << content_;
        }

        std::filesystem::path path() const
        {
            return directory_.path() / "file.bin";
        }

        TemporaryDirectory directory_{temporaryRoot, true};
        std::string content_{};
    };

    TEST_F(SharedInputFileStreamTests, ReadsTheFileInChunks)
    {
        SharedInputFileStream stream;
        stream.open(path());
        ASSERT_TRUE(stream.is_open());

        std::string read;
        std::array<char, 7'000> chunk{};
        while (stream.read(chunk.data(), chunk.size()) || stream.gcount() > 0)
            read.append(chunk.data(), static_cast<std::size_t>(stream.gcount()));

        EXPECT_EQ(read, content_);
        EXPECT_TRUE(stream.eof());
    }

    TEST_F(SharedInputFileStreamTests, SeeksAndTellsLikeAFileStream)
    {
        SharedInputFileStream stream;
        stream.open(path());

        stream.seekg(0, std::ios::end);
        EXPECT_EQ(static_cast<std::size_t>(stream.tellg()), content_.size());
        stream.seekg(0, std::ios::beg);
        std::array<char, 3> start{};
        ASSERT_TRUE(stream.read(start.data(), start.size()));
        EXPECT_EQ(std::string(start.data(), start.size()), "abc");
        EXPECT_EQ(stream.tellg(), std::streampos{3});
        stream.seekg(100'000);
        std::array<char, 2> middle{};
        ASSERT_TRUE(stream.read(middle.data(), middle.size()));
        EXPECT_EQ(std::string(middle.data(), middle.size()), content_.substr(100'000, 2));
        EXPECT_TRUE(stream.good());
    }

    TEST_F(SharedInputFileStreamTests, ReadingPastTheEndFailsWithWhatThereWas)
    {
        SharedInputFileStream stream;
        stream.open(path());
        stream.seekg(static_cast<std::streamoff>(content_.size() - 4));

        std::array<char, 10> tail{};
        stream.read(tail.data(), tail.size());

        EXPECT_EQ(stream.gcount(), 4);
        EXPECT_FALSE(stream.good());
        EXPECT_EQ(std::string(tail.data(), 4), content_.substr(content_.size() - 4));
    }

    TEST_F(SharedInputFileStreamTests, TheFileCanBeRenamedAndDeletedWhileItIsOpen)
    {
        SharedInputFileStream stream;
        stream.open(path());
        ASSERT_TRUE(stream.is_open());

        std::error_code renameError;
        std::filesystem::rename(path(), directory_.path() / "renamed.bin", renameError);
        EXPECT_FALSE(renameError) << renameError.message();
        std::error_code removeError;
        std::filesystem::remove(directory_.path() / "renamed.bin", removeError);
        EXPECT_FALSE(removeError) << removeError.message();

        std::array<char, 5> start{};
        ASSERT_TRUE(stream.read(start.data(), start.size()));
        EXPECT_EQ(std::string(start.data(), start.size()), "abcde");
        stream.close();
        EXPECT_FALSE(stream.is_open());
    }

    TEST_F(SharedInputFileStreamTests, AMissingFileFailsToOpen)
    {
        SharedInputFileStream stream;
        stream.open(directory_.path() / "missing.bin");

        EXPECT_FALSE(stream.is_open());
        EXPECT_TRUE(stream.fail());
    }
}
