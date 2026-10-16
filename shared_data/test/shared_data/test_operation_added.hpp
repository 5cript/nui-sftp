#pragma once

#include <shared_data/file_operations/operation_added.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>

namespace SharedData::Test
{
    TEST(OperationAddedTests, RenameCarriesRemoteSourceAndDestination)
    {
        const OperationAdded added{
            .operationId = Ids::generateOperationId(),
            .type = OperationType::Rename,
            .source = Paths::RemotePath::make("/srv/old name").value(),
            .destination = Paths::RemotePath::make("/srv/new name").value(),
        };
        const nlohmann::json json = added;
        const auto read = json.get<OperationAdded>();
        ASSERT_TRUE(read.source.has_value());
        ASSERT_TRUE(read.destination.has_value());
        EXPECT_EQ(read.source->value(), "/srv/old name");
        EXPECT_EQ(read.destination->value(), "/srv/new name");
        EXPECT_FALSE(read.localPath.has_value());
        EXPECT_FALSE(read.remotePath.has_value());
    }

    TEST(OperationAddedTests, InvalidPathsAreRefusedOnRead)
    {
        nlohmann::json json =
            OperationAdded{.operationId = Ids::generateOperationId(), .type = OperationType::Download};
        json["remotePath"] = "relative/file";
        EXPECT_THROW(json.get<OperationAdded>(), std::exception);
    }
}
