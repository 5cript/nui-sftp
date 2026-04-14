#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace SharedData
{
    /**
     * @brief One item in a bulk-add request. The frontend is authoritative:
     *        sizes are trusted and no filesystem probe happens on the backend
     *        for file entries. Directories are split off into their own Scan
     *        + Bulk operation pair during backend dispatch.
     */
    struct BulkAddEntry
    {
        std::filesystem::path src{};
        std::filesystem::path dst{};       ///< Unused for delete.
        std::uint64_t sizeBytes{0};        ///< Meaningful only when !isDirectory.
        bool isDirectory{false};
    };
    BOOST_DESCRIBE_STRUCT(BulkAddEntry, (), (src, dst, sizeBytes, isDirectory))

    /**
     * @brief Single-RPC payload for adding many file operations at once.
     *        Backend partitions entries into files (→ one aggregate Bulk
     *        operation) and directories (→ one Scan+Bulk pair each), and
     *        creates them all within a single strand dispatch to amortize
     *        SSH context switching.
     */
    struct BulkAddRequest
    {
        std::vector<BulkAddEntry> entries{};
        bool allowOverwrite{false};
        bool insertRefresh{false};
        OperationMode mode{OperationMode::Queued};
    };
    BOOST_DESCRIBE_STRUCT(BulkAddRequest, (), (entries, allowOverwrite, insertRefresh, mode))
}
