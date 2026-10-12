#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

namespace SharedData
{
    struct OperationAdded
    {
        Ids::OperationId operationId;
        OperationType type;
        OperationMode mode{OperationMode::Queued};
        bool insertRefresh{false};
        std::optional<std::uint64_t> totalBytes{std::nullopt};
        std::optional<std::filesystem::path> localPath{};
        std::optional<std::filesystem::path> remotePath{};
        /** @brief The operation scans before it starts its own work. */
        std::optional<bool> scanPhase{};
    };
    BOOST_DESCRIBE_STRUCT(
        OperationAdded,
        (),
        (operationId,
         type,
         mode,
         totalBytes,
         localPath,
         remotePath,
         source,
         destination,
         insertRefresh,
         scanPhase,
         allowOverwrite)
    )
}