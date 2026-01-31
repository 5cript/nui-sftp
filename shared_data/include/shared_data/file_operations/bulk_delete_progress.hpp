#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace SharedData
{
    struct BulkDeleteProgress
    {
        Ids::OperationId operationId;
        std::string currentFile;
        std::uint64_t filesDeleted;
        std::uint64_t totalFiles;
    };
    BOOST_DESCRIBE_STRUCT(BulkDeleteProgress, (), (operationId, currentFile, filesDeleted, totalFiles))
}