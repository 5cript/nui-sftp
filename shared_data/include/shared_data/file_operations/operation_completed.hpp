#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <utility/describe.hpp>
#include <utility/enum_string_convert.hpp>
#include <shared_data/time_point.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <filesystem>
#include <chrono>

namespace SharedData
{
    BOOST_DEFINE_ENUM_CLASS(OperationCompletionReason, Completed, Canceled, Failed)

    struct OperationCompleted
    {
        OperationCompletionReason reason;
        Ids::OperationId operationId;
        std::chrono::system_clock::time_point completionTime;
        std::optional<std::filesystem::path> localPath{std::nullopt};
        std::optional<std::filesystem::path> remotePath{std::nullopt};
        std::optional<OperationError> error{std::nullopt};
        std::vector<std::pair<std::filesystem::path, OperationError>> failedEntries{};
    };
    BOOST_DESCRIBE_STRUCT(
        OperationCompleted,
        (),
        (reason, operationId, completionTime, localPath, remotePath, error, failedEntries)
    )
}