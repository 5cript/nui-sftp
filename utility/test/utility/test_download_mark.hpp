#pragma once

#include <utility/download_mark.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef __APPLE__
#    include <sys/xattr.h>
#endif

extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class DownloadMarkTests : public ::testing::Test
    {
      protected:
        static std::string read(std::filesystem::path const& path)
        {
            std::ifstream file{path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        std::filesystem::path write(std::string const& name)
        {
            const auto path = directory_.path() / name;
            std::ofstream{path, std::ios::binary} << "content";
            return path;
        }

        TemporaryDirectory directory_{temporaryRoot, true};
    };

    TEST_F(DownloadMarkTests, MarkingKeepsTheContent)
    {
        const auto file = write("downloaded.docx");
        EXPECT_FALSE(markAsDownloaded(file));
        EXPECT_EQ(read(file), "content");
    }

#ifdef _WIN32
    TEST_F(DownloadMarkTests, WindowsWritesTheInternetZone)
    {
        const auto file = write("downloaded.docx");
        ASSERT_FALSE(markAsDownloaded(file));
        auto stream = file;
        stream += L":Zone.Identifier";
        EXPECT_EQ(read(stream), "[ZoneTransfer]\r\nZoneId=3\r\n");
    }
#elif defined(__APPLE__)
    TEST_F(DownloadMarkTests, MacSetsTheQuarantineAttribute)
    {
        const auto file = write("downloaded.docx");
        ASSERT_FALSE(markAsDownloaded(file));
        char value[128]{};
        const auto length = ::getxattr(file.c_str(), "com.apple.quarantine", value, sizeof(value) - 1, 0, 0);
        ASSERT_GT(length, 0);
        EXPECT_TRUE(std::string(value, static_cast<std::size_t>(length)).starts_with("0081;"));
    }

    TEST_F(DownloadMarkTests, MacReportsAMissingFile)
    {
        EXPECT_TRUE(markAsDownloaded(directory_.path() / "missing"));
    }
#else
    TEST_F(DownloadMarkTests, LinuxLeavesFilesUnmarked)
    {
        const auto file = write("downloaded.docx");
        EXPECT_FALSE(markAsDownloaded(file));
        EXPECT_FALSE(markAsDownloaded(directory_.path() / "missing")) << "nothing to mark, nothing to fail";
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator{directory_.path()}, {}), 1)
            << "no side file is written";
    }
#endif
}
