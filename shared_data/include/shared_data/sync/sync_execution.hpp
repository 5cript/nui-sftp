#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <utility/describe.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace SharedData::Sync
{
    /**
     * @brief Asks the backend to plan and queue the selected rows of one section. The request names rows only, the
     *        backend resolves every row below its own roots.
     *
     * The operation ids are chosen by the frontend, so it can listen to them before the reply arrives. An id is
     * unused when its part of the plan is empty.
     */
    struct SyncExecutionRequest
    {
        Ids::SyncSessionId syncSessionId{};
        std::uint64_t generation{0};
        DiffSection section{DiffSection::Upload};
        /** @brief Sparse: a key selects its row and everything below it. */
        std::vector<std::string> relKeys{};
        OperationMode mode{OperationMode::Queued};
        Ids::OperationId uploadId{};
        Ids::OperationId downloadId{};
        Ids::OperationId remoteDeleteId{};
        Ids::OperationId localDeleteId{};
    };
    BOOST_DESCRIBE_STRUCT(
        SyncExecutionRequest,
        (),
        (syncSessionId, generation, section, relKeys, mode, uploadId, downloadId, remoteDeleteId, localDeleteId)
    )

    /**
     * @brief Which rows each queued operation covers.
     */
    struct SyncExecutionResult
    {
        /** @brief Rows of the bulk upload, in the order of its entry indices, by which it reports failures. */
        std::vector<std::string> uploads{};
        std::vector<std::string> downloads{};
        /** @brief Every listed item of a directory delete is a row of its own, its content after it. */
        std::vector<std::string> remoteDeletes{};
        /** @brief The path each delete row reports when it fails, in the same order. */
        std::vector<std::string> remoteDeleteSources{};
        std::vector<std::string> localDeletes{};
        std::vector<std::string> localDeleteSources{};
        /** @brief Rows that were refused or failed right away, nothing was queued for them. */
        std::vector<std::string> failed{};
    };
    BOOST_DESCRIBE_STRUCT(
        SyncExecutionResult,
        (),
        (uploads,
         downloads,
         remoteDeletes,
         remoteDeleteSources,
         localDeletes,
         localDeleteSources,
         failed)
    )

    inline void to_json(nlohmann::json& json, SyncExecutionRequest const& request)
    {
        SharedData::to_json(json, request);
    }
    inline void from_json(nlohmann::json const& json, SyncExecutionRequest& request)
    {
        SharedData::from_json(json, request);
    }
    inline void to_json(nlohmann::json& json, SyncExecutionResult const& result)
    {
        SharedData::to_json(json, result);
    }
    inline void from_json(nlohmann::json const& json, SyncExecutionResult& result)
    {
        SharedData::from_json(json, result);
    }
}
