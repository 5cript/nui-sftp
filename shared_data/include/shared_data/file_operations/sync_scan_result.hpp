#pragma once

#include <shared_data/directory_entry.hpp>
#include <utility/describe.hpp>
#include <ids/ids.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace SharedData
{
    struct SyncScanResult
    {
        Ids::OperationId operationId{};
        bool isLocal{false};
        std::uint64_t totalBytes{0};
        std::vector<SharedData::DirectoryEntry> entries{};
    };

    BOOST_DESCRIBE_STRUCT(SyncScanResult, (), (operationId, isLocal, totalBytes, entries))

    inline void to_json(nlohmann::json& j, SyncScanResult const& res)
    {
        j = nlohmann::json{
            {"operationId", res.operationId},
            {"isLocal", res.isLocal},
            {"totalBytes", res.totalBytes},
            {"entries", res.entries},
        };
    }

    inline void from_json(nlohmann::json const& j, SyncScanResult& res)
    {
        j.at("operationId").get_to(res.operationId);
        j.at("isLocal").get_to(res.isLocal);
        j.at("totalBytes").get_to(res.totalBytes);
        j.at("entries").get_to(res.entries);
    }
}
