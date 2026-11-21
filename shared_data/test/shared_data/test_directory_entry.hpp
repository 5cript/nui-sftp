#pragma once

#include <shared_data/directory_entry.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>

namespace SharedData::Test
{
    TEST(DirectoryEntryTests, TheTimePrecisionRoundTripsThroughJson)
    {
        for (const auto precision : {TimePrecision::Second, TimePrecision::Minute, TimePrecision::Day})
        {
            const DirectoryEntry entry{
                .name = "listed.txt",
                .type = FileType::Regular,
                .mtime = 1699999980u,
                .mtimePrecision = precision,
            };
            const nlohmann::json json = entry;
            EXPECT_EQ(json.get<DirectoryEntry>().mtimePrecision, precision) << json.dump();
        }
    }

    TEST(DirectoryEntryTests, AnUnknownTimePrecisionIsRefusedOnRead)
    {
        nlohmann::json json = DirectoryEntry{.name = "listed.txt", .type = FileType::Regular};
        json["mtimePrecision"] = "Hour";
        EXPECT_THROW(json.get<DirectoryEntry>(), std::invalid_argument);
    }
}
