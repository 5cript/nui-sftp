#pragma once

#include <shared_data/file_operations/bulk_transfer_request.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace SharedData::Test
{
    namespace
    {
#ifdef _WIN32
        constexpr char const* localDirectory = "C:/Users/test";
#else
        constexpr char const* localDirectory = "/home/test";
#endif

        Paths::LocalPath local(std::string const& below)
        {
            return Paths::LocalPath::fromUtf8(std::string{localDirectory} + "/" + below).value();
        }

        Paths::RemotePath remote(std::string const& path)
        {
            return Paths::RemotePath::make(path).value();
        }
    }

    TEST(BulkTransferRequestTests, UploadRoundTripsThroughJson)
    {
        const BulkUploadRequest request{
            .entries =
                {
                    {.source = local("a.txt"), .destination = remote("/srv/a\\b"), .sizeBytes = 3},
                    {.source = local("dir"), .destination = remote("/srv/dir"), .isDirectory = true},
                },
            .allowOverwrite = true,
            .insertRefresh = true,
            .mode = OperationMode::PriorityQueued,
        };
        const nlohmann::json json = request;
        const auto read = json.get<BulkUploadRequest>();
        ASSERT_EQ(read.entries.size(), 2u);
        EXPECT_EQ(read.entries[0].source, local("a.txt"));
        EXPECT_EQ(read.entries[0].destination.value(), "/srv/a\\b");
        EXPECT_EQ(read.entries[0].sizeBytes, 3u);
        EXPECT_TRUE(read.entries[1].isDirectory);
        EXPECT_TRUE(read.allowOverwrite);
        EXPECT_TRUE(read.insertRefresh);
        EXPECT_EQ(read.mode, OperationMode::PriorityQueued);
    }

    TEST(BulkTransferRequestTests, DownloadRoundTripsThroughJson)
    {
        const BulkDownloadRequest request{
            .entries =
                {
                    {.source = remote("/srv/a.txt"),
                     .destination = local("a.txt"),
                     .sizeBytes = 7,
                     .mtime = 1700000000u,
                     .mtimeNsec = 5u},
                },
        };
        const nlohmann::json json = request;
        const auto read = json.get<BulkDownloadRequest>();
        ASSERT_EQ(read.entries.size(), 1u);
        EXPECT_EQ(read.entries[0].source.value(), "/srv/a.txt");
        EXPECT_EQ(read.entries[0].destination, local("a.txt"));
        EXPECT_EQ(read.entries[0].sizeBytes, 7u);
        EXPECT_EQ(read.entries[0].mtime, 1700000000u);
        EXPECT_EQ(read.entries[0].mtimeNsec, 5u);
    }

    TEST(BulkTransferRequestTests, InvalidPathsAreRefusedOnRead)
    {
        for (auto const* remotePath : {"srv/a", "/srv/../etc", ""})
        {
            nlohmann::json upload =
                BulkUploadRequest{.entries = {{.source = local("a"), .destination = remote("/srv/a")}}};
            upload["entries"][0]["destination"] = remotePath;
            EXPECT_THROW(upload.get<BulkUploadRequest>(), std::exception) << remotePath;

            nlohmann::json download =
                BulkDownloadRequest{.entries = {{.source = remote("/srv/a"), .destination = local("a")}}};
            download["entries"][0]["source"] = remotePath;
            EXPECT_THROW(download.get<BulkDownloadRequest>(), std::exception) << remotePath;
        }

        for (auto const* localPath : {"relative/a", ""})
        {
            nlohmann::json upload =
                BulkUploadRequest{.entries = {{.source = local("a"), .destination = remote("/srv/a")}}};
            upload["entries"][0]["source"] = localPath;
            EXPECT_THROW(upload.get<BulkUploadRequest>(), std::exception) << localPath;

            nlohmann::json download =
                BulkDownloadRequest{.entries = {{.source = remote("/srv/a"), .destination = local("a")}}};
            download["entries"][0]["destination"] = localPath;
            EXPECT_THROW(download.get<BulkDownloadRequest>(), std::exception) << localPath;
        }
    }
}
