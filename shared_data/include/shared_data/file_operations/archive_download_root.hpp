#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/directory_entry.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <nlohmann/json.hpp>

namespace SharedData
{
    /**
     * @brief One selected remote item of an archive download, packed under its own name. A directory is scanned
     *        and packed with its content.
     */
    struct ArchiveDownloadRoot
    {
        Paths::RemotePath path{};
        /** @brief The listing entry of @ref path: type, size, permissions and times for the tar header. */
        DirectoryEntry entry{};
    };
    BOOST_DESCRIBE_STRUCT(ArchiveDownloadRoot, (), (path, entry))
}
