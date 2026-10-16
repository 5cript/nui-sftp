#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace FileTracking
{
    /**
     * @brief The hidden directory inside an instance directory that holds the instance lock and the metadata, so
     *        they never share a name with the tracked content next to them.
     */
    inline constexpr std::string_view bookkeepingDirectoryName{".nui-sftp-tracking"};

    /**
     * @brief The lock file and the metadata file of one instance directory.
     */
    struct BookkeepingPaths
    {
        std::filesystem::path lock;
        std::filesystem::path metadata;
    };

    /**
     * @brief Where an instance keeps its lock and metadata: inside the bookkeeping directory.
     * @param instanceDirectory The instance directory.
     */
    BookkeepingPaths bookkeepingPaths(std::filesystem::path const& instanceDirectory);

    /**
     * @brief Where builds without the bookkeeping directory keep them: "<instance id>.lock" and "metadata.json"
     *        directly in the instance directory. They are still read, and the metadata is mirrored into them.
     * @param instanceDirectory The instance directory, named after the instance id.
     */
    BookkeepingPaths legacyBookkeepingPaths(std::filesystem::path const& instanceDirectory);

    /**
     * @brief Creates the bookkeeping directory of @p instanceDirectory (and the instance directory). On Windows it
     *        gets the hidden attribute, elsewhere the leading dot hides it.
     */
    std::expected<void, std::string> createBookkeepingDirectory(std::filesystem::path const& instanceDirectory);

    /**
     * @brief Whether @p path is the bookkeeping directory of @p instanceDirectory or lies inside it. Compares
     *        lexically, so both paths must be spelled alike (both canonical, for one).
     */
    bool isBookkeepingPath(std::filesystem::path const& instanceDirectory, std::filesystem::path const& path);

    /**
     * @brief The lock file of @p instanceDirectory: the one in the bookkeeping directory, else the legacy one.
     * @return Nothing when neither exists, the directory is then no instance directory.
     */
    std::optional<std::filesystem::path> existingLockFile(std::filesystem::path const& instanceDirectory);

    /**
     * @brief Reads the metadata of @p instanceDirectory: the file in the bookkeeping directory, else the legacy one
     *        of a legacy instance (a legacy lock and no current one; elsewhere "metadata.json" is tracked content).
     * @return Nothing when neither exists, an error when the file cannot be read or parsed.
     */
    std::expected<std::optional<nlohmann::json>, std::string>
    readMetadata(std::filesystem::path const& instanceDirectory);

    /**
     * @brief Writes @p metadata into the bookkeeping directory, creating it when missing, and into the legacy file of
     *        a legacy instance when it has one, so builds that only know the legacy location read the same.
     */
    std::expected<void, std::string>
    writeMetadata(std::filesystem::path const& instanceDirectory, nlohmann::json const& metadata);
}
