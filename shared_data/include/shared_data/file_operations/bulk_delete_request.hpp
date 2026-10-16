#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace SharedData
{
    /**
     * @brief One remote item of a bulk delete. A directory is scanned and removed with its content.
     */
    struct BulkDeleteEntry
    {
        Paths::RemotePath target{};
        bool isDirectory{false};
        /** @brief Only shown on the card for files. */
        std::uint64_t sizeBytes{0};
        /**
         * @brief What a sync compare saw at the target, which is then only deleted while it still holds that. Set
         *        by the backend's sync only, it is not part of the request the frontend sends.
         */
        std::optional<ComparedState> compared{};
    };
    BOOST_DESCRIBE_STRUCT(BulkDeleteEntry, (), (target, isDirectory, sizeBytes))

    /**
     * @brief Single-RPC payload for deleting many remote items as one delete operation: files are deleted as listed,
     *        every directory is scanned first and deleted with its content.
     */
    struct BulkDeleteRequest
    {
        std::vector<BulkDeleteEntry> entries{};
        bool insertRefresh{false};
        OperationMode mode{OperationMode::Queued};
        /**
         * @brief The entries already list every item to delete, a directory before its content: nothing is
         *        scanned, a directory is only removed once empty.
         */
        bool listedContentOnly{false};
    };
    BOOST_DESCRIBE_STRUCT(BulkDeleteRequest, (), (entries, insertRefresh, mode, listedContentOnly))
}
