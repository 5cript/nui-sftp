#pragma once

#include <shared_data/file_operations/bulk_delete_request.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>

namespace SharedData::Test
{
    TEST(BulkDeleteRequestTests, RoundTripsThroughJson)
    {
        const BulkDeleteRequest request{
            .entries =
                {
                    {.target = Paths::RemotePath::make("/home/test/a\\b").value(), .isDirectory = false, .sizeBytes = 3},
                    {.target = Paths::RemotePath::make("/home/test/dir").value(), .isDirectory = true},
                },
            .insertRefresh = true,
            .mode = OperationMode::PriorityQueued,
        };
        const nlohmann::json json = request;
        const auto read = json.get<BulkDeleteRequest>();
        ASSERT_EQ(read.entries.size(), 2u);
        EXPECT_EQ(read.entries[0].target.value(), "/home/test/a\\b");
        EXPECT_EQ(read.entries[0].sizeBytes, 3u);
        EXPECT_TRUE(read.entries[1].isDirectory);
        EXPECT_TRUE(read.insertRefresh);
        EXPECT_EQ(read.mode, OperationMode::PriorityQueued);
    }

    TEST(BulkDeleteRequestTests, RelativeOrParentTargetsAreRefusedOnRead)
    {
        for (auto const* target : {"home/test/a", "/home/test/../etc", ""})
        {
            nlohmann::json json = BulkDeleteRequest{.entries = {{.target = Paths::RemotePath::root()}}};
            json["entries"][0]["target"] = target;
            EXPECT_THROW(json.get<BulkDeleteRequest>(), std::exception) << target;
        }
    }
}
