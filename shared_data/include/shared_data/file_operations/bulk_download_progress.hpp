#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace SharedData
{
    struct BulkDownloadProgress
    {
        Ids::OperationId operationId;
        std::string currentFile;
        std::uint64_t fileCurrentIndex;
        std::uint64_t fileCount;
        std::uint64_t currentFileBytes;
        std::uint64_t currentFileTotalBytes;
        std::uint64_t bytesCurrent;
        std::uint64_t bytesTotal;
        std::make_signed_t<std::size_t> bytesPerSecond;
    };
    BOOST_DESCRIBE_STRUCT(
        BulkDownloadProgress,
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