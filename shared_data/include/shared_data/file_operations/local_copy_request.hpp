#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <vector>

namespace SharedData
{
    /**
     * @brief One local item that is copied or moved to another local place, a directory with its content.
     */
    struct LocalCopyEntry
    {
        Paths::LocalPath source{};
        /** @brief Where the item ends up, the source itself copies it next to itself under a free name. */
        Paths::LocalPath destination{};
        /** @brief What the explorer listed, only shown. */
        std::uint64_t sizeBytes{0};
        bool isDirectory{false};
        /**
         * @brief The user agreed to replace this existing destination, a directory is merged with it. Without it a
         *        destination that exists is not touched and the entry fails.
         */
        bool mayOverwrite{false};
    };
    BOOST_DESCRIBE_STRUCT(LocalCopyEntry, (), (source, destination, sizeBytes, isDirectory, mayOverwrite))

    /**
     * @brief Copies or moves local items in the local operation queue.
     */
    struct LocalCopyRequest
    {
        std::vector<LocalCopyEntry> entries{};
        /** @brief The sources go away once they are at their destination. */
        bool move{false};
    };
    BOOST_DESCRIBE_STRUCT(LocalCopyRequest, (), (entries, move))
}
