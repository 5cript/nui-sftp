#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace SharedData
{
    /**
     * @brief One file currently being transferred by a bulk operation.
     */
    struct BulkFileProgress
    {
        std::string file;
        // Position of the file in the bulk's entry list. Stable for the lifetime of the bulk.
        std::uint64_t entryIndex;
        std::uint64_t bytes;
        std::uint64_t totalBytes;
    };
    BOOST_DESCRIBE_STRUCT(BulkFileProgress, (), (file, entryIndex, bytes, totalBytes))

    struct BulkProgress
    {
        Ids::OperationId operationId;
        // Entries (files and directories) the bulk has finished with.
        std::uint64_t fileCurrentIndex;
        std::uint64_t fileCount;
        std::uint64_t bytesCurrent;
        std::uint64_t bytesTotal;
        // See TransferProgress::bytesPerSecond for the reason this is
        // pinned to int64 (describe-based split-u64 encoding symmetry).
        std::int64_t bytesPerSecond;
        // Files being transferred or waiting for a transfer buffer right now.
        std::vector<BulkFileProgress> inFlight;
        // Entries that failed with a skippable error and were left out; cumulative. 32-bit
        // because the frontend converts integer vectors through a typed array, and a 64-bit
        // view rejects the plain numbers the backend sends.
        std::vector<std::uint32_t> failedEntryIndices;
    };
    BOOST_DESCRIBE_STRUCT(
        BulkProgress,
        (),
        (operationId, fileCurrentIndex, fileCount, bytesCurrent, bytesTotal, bytesPerSecond, inFlight, failedEntryIndices)
    )
}
