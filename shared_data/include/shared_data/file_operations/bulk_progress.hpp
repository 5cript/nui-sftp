#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace SharedData
{
    struct BulkProgress
    {
        Ids::OperationId operationId;
        std::string currentFile;
        std::uint64_t fileCurrentIndex;
        std::uint64_t fileCount;
        std::uint64_t currentFileBytes;
        std::uint64_t currentFileTotalBytes;
        std::uint64_t bytesCurrent;
        std::uint64_t bytesTotal;
        // See TransferProgress::bytesPerSecond for the reason this is
        // pinned to int64 (describe-based split-u64 encoding symmetry).
        std::int64_t bytesPerSecond;
    };
    BOOST_DESCRIBE_STRUCT(
        BulkProgress,
        (),
        (operationId,
            currentFile,
            fileCurrentIndex,
            fileCount,
            currentFileBytes,
            currentFileTotalBytes,
            bytesCurrent,
            bytesTotal,
            bytesPerSecond)
    )
}