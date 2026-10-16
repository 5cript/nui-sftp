#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

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
        std::optional<Paths::LocalPath> localPath{};
        std::optional<Paths::RemotePath> remotePath{};
        /** @brief Rename only: the remote item before the rename. */
        std::optional<Paths::RemotePath> source{};
        /** @brief Rename only: where the remote item ends up. */
        std::optional<Paths::RemotePath> destination{};
        /** @brief The operation scans before it starts its own work. */
        std::optional<bool> scanPhase{};
        /** @brief Single transfers: whether the destination may be replaced, a resume asks for the same. */
        std::optional<bool> allowOverwrite{};
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