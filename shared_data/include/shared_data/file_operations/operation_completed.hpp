#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>
#include <utility/enum_string_convert.hpp>
#include <shared_data/time_point.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace SharedData
{
    BOOST_DEFINE_ENUM_CLASS(OperationCompletionReason, Completed, Canceled, Failed)

    /**
     * @brief A skipped entry of a prescanned bulk up- or download, by its index, and why it was skipped.
     */
    struct IndexedFailure
    {
        std::uint32_t index{0};
        OperationErrorType type{OperationErrorType::UnknownError};
    };
    BOOST_DESCRIBE_STRUCT(IndexedFailure, (), (index, type))

    struct OperationCompleted
    {
        OperationCompletionReason reason;
        Ids::OperationId operationId;
        std::chrono::system_clock::time_point completionTime;
        std::optional<Paths::LocalPath> localPath{std::nullopt};
        std::optional<Paths::RemotePath> remotePath{std::nullopt};
        std::optional<OperationError> error{std::nullopt};
        /** @brief The skipped entries of a bulk by their source path, a remote path for downloads. */
        std::vector<std::pair<std::string, OperationError>> failedEntries{};
        /** @brief The skipped entries of a prescanned bulk up- or download by their index. */
        std::vector<IndexedFailure> indexedFailures{};
    };
    BOOST_DESCRIBE_STRUCT(
        OperationCompleted,
        (),
        (reason, operationId, completionTime, localPath, remotePath, error, failedEntries, indexedFailures)
    )
}