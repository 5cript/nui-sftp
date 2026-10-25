#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SharedData
{
    /**
     * @brief An instance directory of a downloaded copy that no live process holds any more.
     */
    struct OrphanedCopy
    {
        Ids::InstanceId instanceId;
        /** @brief The name of its first content entry. */
        std::string name;
        /** @brief The instance directory. */
        Paths::LocalPath directory;
        /** @brief Its first content entry, nothing when it holds none. */
        std::optional<Paths::LocalPath> copyPath{};
        /** @brief Where the copy came from, nothing for an instance that did not record it. */
        std::optional<Paths::RemotePath> remotePath{};
        /** @brief The size of its content. */
        std::uint64_t sizeBytes{0};
        /** @brief When the instance was created, seconds since the Unix epoch. */
        std::optional<std::uint64_t> downloadedAt{};
        /** @brief When the instance ended, seconds since the Unix epoch. */
        std::optional<std::uint64_t> endedAt{};
    };
    BOOST_DESCRIBE_STRUCT(
        OrphanedCopy,
        (),
        (instanceId, name, directory, copyPath, remotePath, sizeBytes, downloadedAt, endedAt)
    )

    struct OrphanedCopies
    {
        std::vector<OrphanedCopy> copies{};
    };
    BOOST_DESCRIBE_STRUCT(OrphanedCopies, (), (copies))
}
